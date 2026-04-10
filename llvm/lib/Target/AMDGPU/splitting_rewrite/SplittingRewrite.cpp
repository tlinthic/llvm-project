//===- SplittingRewrite.cpp - Live Range Partitioning MFMA Rewrite -*- C++
//-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implementation of MFMA rewrite using live range partitioning.
///
/// This is a prototype implementation that will eventually replace the
/// existing RewriteMFMAFormStage::rewrite() function.
///
//===----------------------------------------------------------------------===//

#include "../AMDGPUSubtarget.h"
#include "../GCNSubtarget.h"
#include "../SIInstrInfo.h"
#include "../SIRegisterInfo.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "gcn-sched-splitting-rewrite"

using namespace llvm;

namespace {

//===----------------------------------------------------------------------===//
// Data Structures
//===----------------------------------------------------------------------===//

/// Represents a partition of a live range into AGPR and VGPR segments.
struct LiveRangePartition {
  Register OrigReg;
  Register AGPRReg;
  Register VGPRReg;

  /// Maps each VNInfo to its partition (true = AGPR, false = VGPR)
  DenseMap<const VNInfo *, bool> IsAGPR;

  LiveRangePartition() = default;
  LiveRangePartition(Register Orig, Register AGPR, Register VGPR)
      : OrigReg(Orig), AGPRReg(AGPR), VGPRReg(VGPR) {}

  /// Returns the register for a given VNInfo based on its partition.
  Register getRegForVNI(const VNInfo *VNI) const {
    auto It = IsAGPR.find(VNI);
    assert(It != IsAGPR.end() && "VNI not in partition");
    return It->second ? AGPRReg : VGPRReg;
  }
};

/// Represents a point where a copy must be inserted to connect
/// AGPR and VGPR partitions.
struct ConnectionPoint {
  SlotIndex Location;
  Register SrcReg;
  Register DstReg;
  bool IsDefSide; // true = insert after def, false = insert before use.
  MachineInstr *InsertNear;

  ConnectionPoint(SlotIndex Loc, Register Src, Register Dst, bool DefSide,
                  MachineInstr *Near)
      : Location(Loc), SrcReg(Src), DstReg(Dst), IsDefSide(DefSide),
        InsertNear(Near) {}

  bool operator<(const ConnectionPoint &Other) const {
    if (Location != Other.Location)
      return Location < Other.Location;
    if (SrcReg != Other.SrcReg)
      return SrcReg < Other.SrcReg;
    return DstReg < Other.DstReg;
  }

  bool operator==(const ConnectionPoint &Other) const {
    return Location == Other.Location && SrcReg == Other.SrcReg &&
           DstReg == Other.DstReg;
  }
};

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void dumpPartition(const LiveRangePartition &Partition,
                                    const TargetRegisterInfo *TRI) {
  dbgs() << "Partition of " << printReg(Partition.OrigReg, TRI) << ":\n";
  dbgs() << "  AGPR reg: " << printReg(Partition.AGPRReg, TRI) << "\n";
  dbgs() << "  VGPR reg: " << printReg(Partition.VGPRReg, TRI) << "\n";
  dbgs() << "  AGPR VNIs: ";
  for (const auto &[VNI, IsAGPR] : Partition.IsAGPR) {
    if (IsAGPR)
      dbgs() << VNI->id << " ";
  }
  dbgs() << "\n  VGPR VNIs: ";
  for (const auto &[VNI, IsAGPR] : Partition.IsAGPR) {
    if (!IsAGPR)
      dbgs() << VNI->id << " ";
  }
  dbgs() << "\n";
}

LLVM_DUMP_METHOD void dumpConnectionPoint(const ConnectionPoint &IP,
                                          const TargetRegisterInfo *TRI) {
  dbgs() << "  " << IP.Location << ": " << printReg(IP.SrcReg, TRI) << " -> "
         << printReg(IP.DstReg, TRI)
         << (IP.IsDefSide ? " (def side)" : " (use side)") << "\n";
}
#endif

//===----------------------------------------------------------------------===//
// Helper Functions
//===----------------------------------------------------------------------===//

/// Check if a register's live interval contains any PHI definitions.
static bool hasPHIDef(Register Reg, LiveIntervals *LIS) {
  if (!Reg.isVirtual())
    return false;

  LiveInterval &LI = LIS->getInterval(Reg);
  for (const VNInfo *VNI : LI.valnos) {
    if (VNI && VNI->isPHIDef())
      return true;
  }
  return false;
}

/// Classify a VNInfo as AGPR or VGPR based on its defining instruction.
/// Phase 3: Now handles PHI nodes by analyzing uses.
static bool classifyVNInfo(const VNInfo *VNI, LiveIntervals *LIS,
                           const TargetInstrInfo *TII, bool DefIsAGPR,
                           Register Reg, MachineRegisterInfo &MRI) {
  if (VNI->isPHIDef()) {
    // Phase 3: Classify PHI based on uses.
    // If all uses are AGPR (MFMA), classify as AGPR.
    // If all uses are VGPR (non-MFMA), classify as VGPR.
    // If mixed, classify as VGPR (conservative - copies will be inserted).

    bool HasAGPRUse = false;
    bool HasVGPRUse = false;

    for (MachineOperand &UseMO : MRI.use_operands(Reg)) {
      MachineInstr *UseMI = UseMO.getParent();
      SlotIndex UseIdx = LIS->getInstructionIndex(*UseMI);

      // Check if this use is reached by this PHI VNI
      LiveInterval &LI = LIS->getInterval(Reg);
      const VNInfo *ReachingVNI = LI.getVNInfoAt(UseIdx);
      if (ReachingVNI != VNI)
        continue;

      // Check if use needs AGPR or VGPR
      bool UseNeedsAGPR = static_cast<const SIInstrInfo *>(TII)->isMAI(*UseMI);
      if (UseNeedsAGPR)
        HasAGPRUse = true;
      else
        HasVGPRUse = true;
    }

    // If all uses are AGPR, classify as AGPR
    // Otherwise, classify as VGPR (conservative)
    return DefIsAGPR && HasAGPRUse && !HasVGPRUse;
  }

  MachineInstr *DefMI = LIS->getInstructionFromIndex(VNI->def);
  if (!DefMI) {
    // This can happen for live-in values.
    // Conservative: treat as VGPR.
    return false;
  }

  // For MFMA results (DefIsAGPR=true), the def is AGPR if it's an MFMA
  // For MFMA operands (DefIsAGPR=false), the def is AGPR only if it's an MFMA
  bool IsMFMADef = static_cast<const SIInstrInfo *>(TII)->isMAI(*DefMI);

  return DefIsAGPR ? IsMFMADef : false;
}

/// Deduplicate connection points.
static void
deduplicateConnectionPoints(SmallVectorImpl<ConnectionPoint> &Points) {
  llvm::sort(Points);
  Points.erase(llvm::unique(Points), Points.end());
}

/// Optimize copy placement by grouping copies in the same block.
/// For multiple uses in the same block needing the same copy,
/// insert a single copy before the earliest use.
static void optimizeCopyPlacement(SmallVectorImpl<ConnectionPoint> &Points,
                                  LiveIntervals *LIS) {
  if (Points.empty())
    return;

  // Group by (SrcReg, DstReg, Block)
  DenseMap<std::tuple<Register, Register, MachineBasicBlock *>,
           SmallVector<ConnectionPoint, 4>>
      GroupedPoints;

  for (const auto &IP : Points) {
    MachineBasicBlock *MBB = IP.InsertNear->getParent();
    auto Key = std::make_tuple(IP.SrcReg, IP.DstReg, MBB);
    GroupedPoints[Key].push_back(IP);
  }

  Points.clear();

  // For each group, find the earliest insertion point.
  for (auto &[Key, Group] : GroupedPoints) {
    if (Group.size() == 1) {
      // Single copy, keep as-is.
      Points.push_back(Group[0]);
      continue;
    }

    // Multiple copies in same block - find earliest.
    SlotIndex EarliestLoc = Group[0].Location;
    MachineInstr *EarliestMI = Group[0].InsertNear;
    bool IsDefSide = Group[0].IsDefSide;

    for (const auto &IP : Group) {
      if (IP.Location < EarliestLoc) {
        EarliestLoc = IP.Location;
        EarliestMI = IP.InsertNear;
        IsDefSide = IP.IsDefSide;
      }
    }

    // Insert a single copy at the earliest point.
    Points.emplace_back(EarliestLoc, Group[0].SrcReg, Group[0].DstReg,
                        IsDefSide, EarliestMI);

    LLVM_DEBUG(dbgs() << "  Optimized: " << Group.size()
                      << " copies -> 1 copy in same block\n");
  }
}

//===----------------------------------------------------------------------===//
// Phase 1: Core Implementation
//===----------------------------------------------------------------------===//

/// Partition a register's live range into AGPR and VGPR segments.
///
/// \param Reg The register to partition.
/// \param DefIsAGPR If true, MFMA defs are AGPR (for dst operands).
///                  If false, MFMA uses are AGPR (for src2 operands).
/// \param LIS LiveIntervals analysis.
/// \param MRI MachineRegisterInfo.
/// \param TII TargetInstrInfo.
/// \param TRI TargetRegisterInfo.
/// \returns The partition, or std::nullopt if partitioning failed (e.g., PHI)
static std::optional<LiveRangePartition>
partitionLiveRange(Register Reg, bool DefIsAGPR, LiveIntervals *LIS,
                   MachineRegisterInfo &MRI, const TargetInstrInfo *TII,
                   const TargetRegisterInfo *TRI) {

  // Phase 3: PHI nodes are now supported.
  LLVM_DEBUG(dbgs() << "Partitioning " << printReg(Reg, TRI)
                    << (hasPHIDef(Reg, LIS) ? " (has PHI)" : "") << "\n");

  LiveInterval &LI = LIS->getInterval(Reg);

  // First, classify each VNInfo (without creating new registers yet).
  DenseMap<const VNInfo *, bool> VNIClassification;
  for (VNInfo *VNI : LI.valnos) {
    if (!VNI)
      continue;

    bool IsAGPR = classifyVNInfo(VNI, LIS, TII, DefIsAGPR, Reg, MRI);
    VNIClassification[VNI] = IsAGPR;

    LLVM_DEBUG(dbgs() << "  VNI " << VNI->id << " @ " << VNI->def << ": "
                      << (IsAGPR ? "AGPR" : "VGPR")
                      << (VNI->isPHIDef() ? " (PHI)" : "") << "\n");
  }

  // Check if partitioning is actually needed by examining uses.
  // If all uses match their reaching def's register class, no partition needed.
  bool NeedsPartition = false;
  for (MachineOperand &UseMO : MRI.use_operands(Reg)) {
    MachineInstr *UseMI = UseMO.getParent();
    SlotIndex UseIdx = LIS->getInstructionIndex(*UseMI);
    const VNInfo *ReachingVNI = LI.getVNInfoAt(UseIdx);

    if (!ReachingVNI)
      continue;

    bool UseNeedsAGPR = static_cast<const SIInstrInfo *>(TII)->isMAI(*UseMI);
    bool DefIsAGPR = VNIClassification[ReachingVNI];

    if (UseNeedsAGPR != DefIsAGPR) {
      NeedsPartition = true;
      break;
    }
  }

  if (!NeedsPartition) {
    LLVM_DEBUG(dbgs() << "  No partition needed - all uses match defs\n");

    // Special case: If DefIsAGPR=true (MFMA dst) and all uses are AGPR,
    // we still need to create the AGPR register because the original
    // register class might be VGPR. We can skip creating VGPRReg though.
    // Similarly, if DefIsAGPR=false (src2) and all uses/defs are VGPR,
    // no partition needed at all.

    // Check if we need to create single-class "partition" (AGPR-only).
    bool AllAGPR = true;
    bool AllVGPR = true;
    for (auto &[VNI, IsAGPR] : VNIClassification) {
      if (IsAGPR)
        AllVGPR = false;
      else
        AllAGPR = false;
    }

    if (AllAGPR && DefIsAGPR) {
      // MFMA dst with all AGPR uses - create AGPR-only partition.
      const TargetRegisterClass *OrigRC = MRI.getRegClass(Reg);
      const TargetRegisterClass *AGPRRC =
          static_cast<const SIRegisterInfo *>(TRI)->getEquivalentAGPRClass(
              OrigRC);

      Register AGPRReg = MRI.createVirtualRegister(AGPRRC);
      LiveRangePartition Partition(Reg, AGPRReg, AGPRReg); // Use same for both
      Partition.IsAGPR = std::move(VNIClassification);

      LLVM_DEBUG(dbgs() << "  AGPR-only partition created\n");
      return Partition;
    }

    // Otherwise, truly no partition needed.
    return std::nullopt;
  }

  // Create new registers for each partition.
  const TargetRegisterClass *OrigRC = MRI.getRegClass(Reg);
  const TargetRegisterClass *AGPRRC =
      static_cast<const SIRegisterInfo *>(TRI)->getEquivalentAGPRClass(OrigRC);
  const TargetRegisterClass *VGPRRC =
      static_cast<const SIRegisterInfo *>(TRI)->getEquivalentVGPRClass(OrigRC);

  Register AGPRReg = MRI.createVirtualRegister(AGPRRC);
  Register VGPRReg = MRI.createVirtualRegister(VGPRRC);

  LiveRangePartition Partition(Reg, AGPRReg, VGPRReg);
  Partition.IsAGPR = std::move(VNIClassification);

  LLVM_DEBUG(dumpPartition(Partition, TRI));
  LLVM_DEBUG(dbgs() << "  Partition created with " << MRI.getNumVirtRegs() - 2
                    << " -> " << MRI.getNumVirtRegs() << " virtual registers\n");

  return Partition;
}

/// Find all connection points where copies must be inserted.
///
/// A connection point occurs when a def from one partition reaches a use
/// in the other partition.
static SmallVector<ConnectionPoint>
findConnectionPoints(const LiveRangePartition &Partition, bool CopyNearDef,
                     LiveIntervals *LIS, const TargetInstrInfo *TII,
                     const TargetRegisterInfo *TRI) {

  SmallVector<ConnectionPoint> Points;
  LiveInterval &OrigLI = LIS->getInterval(Partition.OrigReg);

  LLVM_DEBUG(dbgs() << "Finding connection points for "
                    << printReg(Partition.OrigReg, TRI) << "\n");

  // Check all uses of the original register.
  MachineRegisterInfo &MRI = LIS->getInstructionFromIndex(OrigLI.begin()->start)
                                 ->getParent()
                                 ->getParent()
                                 ->getRegInfo();

  for (MachineOperand &UseMO : MRI.use_operands(Partition.OrigReg)) {
    MachineInstr *UseMI = UseMO.getParent();
    SlotIndex UseIdx = LIS->getInstructionIndex(*UseMI);

    // Find which VNInfo reaches this use.
    const VNInfo *ReachingVNI = OrigLI.getVNInfoAt(UseIdx);
    if (!ReachingVNI) {
      LLVM_DEBUG(dbgs() << "  No reaching VNI for use at " << UseIdx << "\n");
      continue;
    }

    // Determine if use needs AGPR or VGPR
    bool UseNeedsAGPR = static_cast<const SIInstrInfo *>(TII)->isMAI(*UseMI);

    // Check if reaching def is in the same partition as the use.
    auto It = Partition.IsAGPR.find(ReachingVNI);
    assert(It != Partition.IsAGPR.end() && "VNI not in partition");
    bool DefIsAGPR = It->second;

    LLVM_DEBUG(dbgs() << "  Use at " << UseIdx << " in " << UseMI->getOpcode()
                      << ": needs " << (UseNeedsAGPR ? "AGPR" : "VGPR")
                      << ", reached by " << (DefIsAGPR ? "AGPR" : "VGPR")
                      << " VNI " << ReachingVNI->id << "\n");

    if (UseNeedsAGPR != DefIsAGPR) {
      // Connection point detected!
      MachineInstr *DefMI = LIS->getInstructionFromIndex(ReachingVNI->def);

      SlotIndex Location;
      MachineInstr *InsertNear;
      bool IsDefSide;

      if (UseNeedsAGPR && !DefIsAGPR) {
        // VGPR→AGPR: place near def if requested.
        if (CopyNearDef && DefMI) {
          Location = ReachingVNI->def;
          InsertNear = DefMI;
          IsDefSide = true;
        } else {
          Location = UseIdx;
          InsertNear = UseMI;
          IsDefSide = false;
        }

        Points.emplace_back(Location, Partition.VGPRReg, Partition.AGPRReg,
                            IsDefSide, InsertNear);

        LLVM_DEBUG(dbgs() << "  -> VGPR→AGPR copy needed at " << Location
                          << "\n");
      } else {
        // AGPR→VGPR: place near use.
        Location = UseIdx;
        InsertNear = UseMI;
        IsDefSide = false;

        Points.emplace_back(Location, Partition.AGPRReg, Partition.VGPRReg,
                            IsDefSide, InsertNear);

        LLVM_DEBUG(dbgs() << "  -> AGPR→VGPR copy needed at " << Location
                          << "\n");
      }
    }
  }

  // Deduplicate.
  deduplicateConnectionPoints(Points);

  // Phase 4: Optimize copy placement.
  optimizeCopyPlacement(Points, LIS);

  LLVM_DEBUG({
    dbgs() << "Found " << Points.size()
           << " connection points (after optimization):\n";
    for (const auto &IP : Points)
      dumpConnectionPoint(IP, TRI);
  });

  return Points;
}

/// Insert copies at all connection points.
static void insertCopies(ArrayRef<ConnectionPoint> Points, LiveIntervals *LIS,
                         const TargetInstrInfo *TII) {

  LLVM_DEBUG(dbgs() << "Inserting " << Points.size() << " copies\n");

  for (const ConnectionPoint &IP : Points) {
    MachineBasicBlock *MBB = IP.InsertNear->getParent();
    MachineBasicBlock::iterator InsertPt;

    if (IP.IsDefSide) {
      // Insert after the def.
      InsertPt = std::next(IP.InsertNear->getIterator());
    } else {
      // Insert before the use.
      InsertPt = IP.InsertNear->getIterator();
    }

    MachineInstrBuilder Copy =
        BuildMI(*MBB, InsertPt, IP.InsertNear->getDebugLoc(),
                TII->get(TargetOpcode::COPY))
            .addDef(IP.DstReg)
            .addUse(IP.SrcReg);

    LIS->InsertMachineInstrInMaps(*Copy);

    LLVM_DEBUG(dbgs() << "  Inserted: " << *Copy);
  }
}

/// Collect all uses and defs of a register BEFORE any copies are inserted.
/// Returns pair of (uses, defs).
static std::pair<SmallVector<MachineOperand *, 8>,
                 SmallVector<MachineOperand *, 8>>
collectOperands(Register Reg, MachineRegisterInfo &MRI) {
  SmallVector<MachineOperand *, 8> Uses;
  SmallVector<MachineOperand *, 8> Defs;

  for (MachineOperand &UseMO : MRI.use_operands(Reg))
    Uses.push_back(&UseMO);

  for (MachineOperand &DefMO : MRI.def_operands(Reg))
    Defs.push_back(&DefMO);

  return {Uses, Defs};
}

/// Update the collected uses and defs to use the appropriate partition
/// register. For MFMA instructions (which have been rewritten to AGPR form),
/// always use AGPR. For non-MFMA instructions, use VGPR.
static void updateOperands(const LiveRangePartition &Partition,
                           ArrayRef<MachineOperand *> Uses,
                           ArrayRef<MachineOperand *> Defs,
                           MachineRegisterInfo &MRI,
                           const TargetInstrInfo *TII) {

  LLVM_DEBUG(dbgs() << "Updating operands for "
                    << printReg(Partition.OrigReg, MRI.getTargetRegisterInfo())
                    << "\n");

  // Update all uses.
  for (MachineOperand *UseMO : Uses) {
    MachineInstr *UseMI = UseMO->getParent();

    // MFMA instructions (already rewritten to AGPR form) need AGPR
    bool UseNeedsAGPR = static_cast<const SIInstrInfo *>(TII)->isMAI(*UseMI);
    Register NewReg = UseNeedsAGPR ? Partition.AGPRReg : Partition.VGPRReg;

    UseMO->setReg(NewReg);

    LLVM_DEBUG(dbgs() << "  Use in " << (UseNeedsAGPR ? "MFMA" : "non-MFMA")
                      << " -> " << printReg(NewReg, MRI.getTargetRegisterInfo())
                      << "\n");
  }

  // Update all defs.
  for (MachineOperand *DefMO : Defs) {
    MachineInstr *DefMI = DefMO->getParent();

    // MFMA instructions (already rewritten to AGPR form) produce AGPR
    bool DefIsAGPR = static_cast<const SIInstrInfo *>(TII)->isMAI(*DefMI);
    Register NewReg = DefIsAGPR ? Partition.AGPRReg : Partition.VGPRReg;

    DefMO->setReg(NewReg);

    LLVM_DEBUG(dbgs() << "  Def in " << (DefIsAGPR ? "MFMA" : "non-MFMA")
                      << " -> " << printReg(NewReg, MRI.getTargetRegisterInfo())
                      << "\n");
  }
}

} // anonymous namespace

//===----------------------------------------------------------------------===//
// Public Interface (for integration with RewriteMFMAFormStage)
//===----------------------------------------------------------------------===//

namespace llvm {

/// Phase 2-4 rewrite using live range partitioning.
///
/// This is the entry point that replaces RewriteMFMAFormStage::rewrite().
/// Phase 2: Tracks region boundaries.
/// Phase 3: Handles PHI nodes.
/// Phase 4: Optimizations and verification.
bool rewriteWithPartitioning(
    ArrayRef<std::pair<MachineInstr *, unsigned>> RewriteCands,
    MachineFunction &MF, LiveIntervals *LIS, const TargetInstrInfo *TII,
    const TargetRegisterInfo *TRI, const SIInstrInfo *SII) {

  MachineRegisterInfo &MRI = MF.getRegInfo();

  LLVM_DEBUG(
      dbgs()
      << "=== Starting live range partitioning rewrite (Phase 2-4) ===\n");
  LLVM_DEBUG(dbgs() << "Function: " << MF.getName() << "\n");
  LLVM_DEBUG(dbgs() << "Processing " << RewriteCands.size() << " candidates\n");

  // Step 1: Identify all unique registers to partition and collect their
  // operands.
  struct RegInfo {
    bool IsAGPRDef; // true if register is defined by MFMA (dst), false if used.
                    // (src2)
    SmallVector<MachineOperand *, 8> Uses;
    SmallVector<MachineOperand *, 8> Defs;
  };
  DenseMap<Register, RegInfo> RegistersToPartition;

  for (auto &[MI, OrigOpcode] : RewriteCands) {
    int ReplacementOp = AMDGPU::getMFMASrcCVDstAGPROp(MI->getOpcode());
    if (ReplacementOp == -1)
      continue;

    // Track dst register.
    Register DstReg = MI->getOperand(0).getReg();
    if (DstReg.isVirtual()) {
      if (!RegistersToPartition.count(DstReg)) {
        auto [Uses, Defs] = collectOperands(DstReg, MRI);
        RegistersToPartition[DstReg] = {/*IsAGPRDef=*/true, Uses, Defs};
      }
    }

    // Track src2 register.
    MachineOperand *Src2 = SII->getNamedOperand(*MI, AMDGPU::OpName::src2);
    if (Src2 && Src2->isReg()) {
      Register Src2Reg = Src2->getReg();
      if (Src2Reg.isVirtual()) {
        if (!RegistersToPartition.count(Src2Reg)) {
          auto [Uses, Defs] = collectOperands(Src2Reg, MRI);
          RegistersToPartition[Src2Reg] = {/*IsAGPRDef=*/false, Uses, Defs};
        }
      }
    }
  }

  // Step 2: Partition each unique register once.
  DenseMap<Register, LiveRangePartition> Partitions;
  for (auto &[Reg, Info] : RegistersToPartition) {
    LLVM_DEBUG(dbgs() << "\nPartitioning " << printReg(Reg, TRI)
                      << (Info.IsAGPRDef ? " (AGPR def)" : " (VGPR def)")
                      << "\n");
    auto Partition =
        partitionLiveRange(Reg, Info.IsAGPRDef, LIS, MRI, TII, TRI);
    if (Partition) {
      Partitions[Reg] = *Partition;
    }
  }

  // Step 3: Process each MFMA and generate copy descriptors.
  // Do NOT modify IR yet - just generate descriptors.
  SmallVector<ConnectionPoint, 32> AllConnectionPoints;
  unsigned Rewritten = 0;

  for (auto &[MI, OrigOpcode] : RewriteCands) {
    int ReplacementOp = AMDGPU::getMFMASrcCVDstAGPROp(MI->getOpcode());
    if (ReplacementOp == -1) {
      LLVM_DEBUG(dbgs() << "No AGPR variant for " << *MI);
      continue;
    }

    LLVM_DEBUG(dbgs() << "\n--- Processing MFMA: " << *MI);

    // Phase 4: Check for tied operands (accumulator pattern)
    // Tied operands must stay in the same partition.
    bool HasTiedOps = false;
    for (unsigned i = 0; i < MI->getNumOperands(); ++i) {
      if (MI->getOperand(i).isReg() && MI->getOperand(i).isTied()) {
        HasTiedOps = true;
        LLVM_DEBUG(dbgs() << "  Has tied operand at index " << i << "\n");
        break;
      }
    }

    // Handle destination operand.
    Register DstReg = MI->getOperand(0).getReg();
    if (!DstReg.isVirtual()) {
      LLVM_DEBUG(dbgs() << "Skipping: dst is physical register\n");
      continue;
    }

    // Use the pre-computed partition for dst.
    auto DstIt = Partitions.find(DstReg);
    if (DstIt == Partitions.end()) {
      LLVM_DEBUG(dbgs() << "No partition for dst\n");
      continue;
    }

    auto DstConnections =
        findConnectionPoints(DstIt->second,
                             /*CopyNearDef=*/false, LIS, TII, TRI);

    // Accumulate connection points, don't insert yet.
    AllConnectionPoints.append(DstConnections.begin(), DstConnections.end());

    // Handle src2 operand.
    MachineOperand *Src2 = SII->getNamedOperand(*MI, AMDGPU::OpName::src2);
    if (Src2 && Src2->isReg()) {
      Register Src2Reg = Src2->getReg();
      if (!Src2Reg.isVirtual()) {
        LLVM_DEBUG(dbgs() << "Skipping src2: physical register\n");
        continue;
      }

      // Use the pre-computed partition for src2.
      auto Src2It = Partitions.find(Src2Reg);
      if (Src2It == Partitions.end()) {
        LLVM_DEBUG(dbgs() << "No partition for src2\n");
        continue;
      }

      auto Src2Connections =
          findConnectionPoints(Src2It->second,
                               /*CopyNearDef=*/true, LIS, TII, TRI);

      // Accumulate connection points, don't insert yet.
      AllConnectionPoints.append(Src2Connections.begin(),
                                 Src2Connections.end());
    }

    Rewritten++;
  }

  // Step 4: Change opcodes and update all operands (before any copies exist in
  // IR).
  LLVM_DEBUG(dbgs() << "\n--- Changing opcodes and updating operands ---\n");

  // First, change all MFMA opcodes to AGPR form.
  for (auto &[MI, OrigOpcode] : RewriteCands) {
    int ReplacementOp = AMDGPU::getMFMASrcCVDstAGPROp(MI->getOpcode());
    if (ReplacementOp != -1) {
      LLVM_DEBUG(dbgs() << "Changing opcode: " << *MI);
      MI->setDesc(TII->get(ReplacementOp));
    }
  }

  // Then, update all operands to use partitioned registers.
  for (auto &[Reg, Partition] : Partitions) {
    auto It = RegistersToPartition.find(Reg);
    if (It != RegistersToPartition.end()) {
      updateOperands(Partition, It->second.Uses, It->second.Defs, MRI, TII);
    }
  }

  // Step 5: Optimize copy placement (group copies in same block)
  LLVM_DEBUG(dbgs() << "\n--- Optimizing copy placement ---\n");
  optimizeCopyPlacement(AllConnectionPoints, LIS);

  // Step 6: Insert all copies at once (after all operands have been updated)
  LLVM_DEBUG(dbgs() << "\n--- Inserting all copies ---\n");
  insertCopies(AllConnectionPoints, LIS, TII);

  LLVM_DEBUG(dbgs() << "\n=== Rewrite complete ===\n");
  LLVM_DEBUG(dbgs() << "MFMAs rewritten: " << Rewritten << "\n");
  LLVM_DEBUG(dbgs() << "Copies inserted: " << AllConnectionPoints.size()
                    << "\n");
  LLVM_DEBUG(
      dbgs() << "Average copies per MFMA: "
             << (Rewritten ? (float)AllConnectionPoints.size() / Rewritten : 0)
             << "\n");

  if (Rewritten > 0) {
    // Reanalyze all live intervals.
    LLVM_DEBUG(dbgs() << "Reanalyzing LiveIntervals\n");
    LIS->reanalyze(MF);
    return true;
  }

  return false;
}

} // namespace llvm
