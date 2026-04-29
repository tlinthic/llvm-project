//===- GCNPartitionMFMARewrite.cpp - Partition-based MFMA Rewrite -*- C++
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
/// This is an alternative implementation that uses VNInfo-based live range
/// partitioning, designed to be more extensible to subregister support.
///
//===----------------------------------------------------------------------===//

#include "AMDGPUSubtarget.h"
#include "GCNSubtarget.h"
#include "SIInstrInfo.h"
#include "SIRegisterInfo.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "gcn-partition-mfma-rewrite"

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
    if (It->second) {
      assert(AGPRReg.isValid() &&
             "AGPR partition requested but no AGPR register created");
      return AGPRReg;
    }
    return VGPRReg;
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
  dbgs() << "  VGPR reg: " << printReg(Partition.VGPRReg, TRI)
         << (Partition.VGPRReg == Partition.OrigReg ? " (original reused)" : "")
         << "\n";
  if (Partition.AGPRReg.isValid()) {
    dbgs() << "  AGPR reg: " << printReg(Partition.AGPRReg, TRI) << "\n";
  } else {
    dbgs() << "  AGPR reg: (none - VGPR only)\n";
  }
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

// Forward declaration
static void findReachingDefs(const VNInfo *VNI, const LiveInterval &LI,
                             LiveIntervals *LIS,
                             SmallVectorImpl<SlotIndex> &DefIdxs);

/// Classify a VNInfo as AGPR or VGPR based on its defining instruction.
/// For PHI nodes, classify based on incoming values (reaching defs).
static bool classifyVNInfo(const VNInfo *VNI, LiveIntervals *LIS,
                           const TargetInstrInfo *TII, bool MFMADefsAreAGPR,
                           Register Reg, MachineRegisterInfo &MRI) {
  if (VNI->isPHIDef()) {
    // Classify PHI based on its incoming values (reaching defs).
    // A PHI should be AGPR if ANY of its incoming values is defined by an MFMA.
    // This handles loop exit PHIs correctly - if the loop produces AGPR values,
    // the exit PHI should also be AGPR.

    LiveInterval &LI = LIS->getInterval(Reg);
    SmallVector<SlotIndex, 8> ReachingDefIdxs;
    // TODO: Cache reaching defs for PHI VNIs (e.g., in LiveRangePartition) to
    // avoid redundant findReachingDefs calls in findConnectionPoints. Profile
    // first to verify the overhead of multiple traversals justifies the extra
    // bookkeeping.
    findReachingDefs(VNI, LI, LIS, ReachingDefIdxs);

    bool HasAGPRReachingDef = false;
    for (SlotIndex RDIdx : ReachingDefIdxs) {
      MachineInstr *RDInstr = LIS->getInstructionFromIndex(RDIdx);
      if (RDInstr && static_cast<const SIInstrInfo *>(TII)->isMAI(*RDInstr)) {
        HasAGPRReachingDef = true;
        break;
      }
    }

    // If register is MFMA dst and has AGPR reaching def, classify as AGPR.
    // Otherwise VGPR (conservative - copies will be inserted at PHI
    // boundaries).
    return MFMADefsAreAGPR && HasAGPRReachingDef;
  }

  MachineInstr *DefMI = LIS->getInstructionFromIndex(VNI->def);
  if (!DefMI) {
    // This can happen for live-in values.
    // Conservative: treat as VGPR.
    return false;
  }

  // MFMA instructions always produce AGPR values (in their dst operand).
  // So any VNInfo whose def is an MFMA should be classified as AGPR,
  // regardless of whether this register is used as MFMA dst or src2.
  bool IsMFMADef = static_cast<const SIInstrInfo *>(TII)->isMAI(*DefMI);

  return IsMFMADef;
}

/// Deduplicate connection points.
static void
deduplicateConnectionPoints(SmallVectorImpl<ConnectionPoint> &Points) {
  llvm::sort(Points);
  Points.erase(llvm::unique(Points), Points.end());
}

/// Find all non-PHI reaching defs for a given VNInfo.
/// If the VNInfo is not a PHI, returns just that def.
/// If the VNInfo is a PHI, traverses backward to find all non-PHI defs.
///
/// TODO: This duplicates RewriteMFMAFormStage::findReachingDefs() in
/// GCNSchedStrategy.cpp with a different signature (takes VNInfo directly
/// instead of MachineOperand). Delete this when integrating the partition-based
/// rewriter into GCNSchedStrategy.cpp.  Please note that the function
/// signatures differ in that this one passes in a VNInfo and the other
/// passes in a MachineOperand and takes VNInfo from that.
static void findReachingDefs(const VNInfo *VNI, const LiveInterval &LI,
                             LiveIntervals *LIS,
                             SmallVectorImpl<SlotIndex> &DefIdxs) {
  LLVM_DEBUG(dbgs() << "  findReachingDefs: VNI " << VNI->id << " @ "
                    << VNI->def << " isPHI=" << VNI->isPHIDef() << "\n");

  // If the def is not a PHI, then it must be the only reaching def.
  if (!VNI->isPHIDef()) {
    DefIdxs.push_back(VNI->def);
    LLVM_DEBUG(dbgs() << "    -> Non-PHI, single def at " << VNI->def << "\n");
    return;
  }

  LLVM_DEBUG(dbgs() << "    -> PHI def, traversing backward...\n");

  MachineBasicBlock *PhiMBB = LIS->getMBBFromIndex(VNI->def);
  SmallPtrSet<MachineBasicBlock *, 8> Visited = {PhiMBB};
  SmallVector<MachineBasicBlock *, 8> Worklist;

  // Mark the predecessor blocks for traversal
  for (MachineBasicBlock *PredMBB : PhiMBB->predecessors()) {
    Worklist.push_back(PredMBB);
    Visited.insert(PredMBB);
  }

  while (!Worklist.empty()) {
    MachineBasicBlock *CurrMBB = Worklist.pop_back_val();

    SlotIndex CurrMBBEnd = LIS->getMBBEndIdx(CurrMBB);
    const VNInfo *CurrVNI = LI.getVNInfoAt(CurrMBBEnd.getPrevSlot());

    if (!CurrVNI)
      continue;

    MachineBasicBlock *DefMBB = LIS->getMBBFromIndex(CurrVNI->def);

    // If there is a def in this block, then add it to the list. This is the
    // reaching def of this path.
    if (!CurrVNI->isPHIDef()) {
      DefIdxs.push_back(CurrVNI->def);
      LLVM_DEBUG(dbgs() << "      Found reaching def at " << CurrVNI->def
                        << "\n");
      continue;
    }

    for (MachineBasicBlock *PredMBB : DefMBB->predecessors()) {
      if (Visited.insert(PredMBB).second)
        Worklist.push_back(PredMBB);
    }
  }
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

    for (const ConnectionPoint &IP : Group) {
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

/// Partition a register's live range into AGPR and VGPR segments.
///
/// \param Reg The register to partition.
/// \param MFMADefsAreAGPR Controls PHI classification: if true, classify PHI
///                        VNInfos as AGPR when they have MFMA reaching defs
///                        (register is an MFMA dst). If false, classify PHI
///                        VNInfos as VGPR (register is only MFMA src2).
///                        Non-PHI VNInfos are always AGPR if defined by MFMA.
/// \param LIS LiveIntervals analysis.
/// \param MRI MachineRegisterInfo.
/// \param TII TargetInstrInfo.
/// \param TRI TargetRegisterInfo.
/// \returns The partition, or std::nullopt if partitioning failed (e.g., PHI)
static std::optional<LiveRangePartition>
partitionLiveRange(Register Reg, bool MFMADefsAreAGPR, LiveIntervals *LIS,
                   MachineRegisterInfo &MRI, const TargetInstrInfo *TII,
                   const TargetRegisterInfo *TRI) {

  LLVM_DEBUG(dbgs() << "Partitioning " << printReg(Reg, TRI)
                    << (hasPHIDef(Reg, LIS) ? " (has PHI)" : "") << "\n");

  LiveInterval &LI = LIS->getInterval(Reg);

  // First, classify each VNInfo (without creating new registers yet).
  DenseMap<const VNInfo *, bool> VNIClassification;
  for (VNInfo *VNI : LI.valnos) {
    if (!VNI)
      continue;

    bool IsAGPR = classifyVNInfo(VNI, LIS, TII, MFMADefsAreAGPR, Reg, MRI);
    VNIClassification[VNI] = IsAGPR;

    LLVM_DEBUG(dbgs() << "  VNI " << VNI->id << " @ " << VNI->def << ": "
                      << (IsAGPR ? "AGPR" : "VGPR")
                      << (VNI->isPHIDef() ? " (PHI)" : "") << "\n");
  }

  // Check if partitioning is needed: do we have BOTH AGPR and VGPR uses,
  // OR both AGPR and VGPR defs? If so, we need to split into two registers.
  bool HasAGPRUse = false;
  bool HasVGPRUse = false;
  unsigned AGPRUseCount = 0;
  unsigned VGPRUseCount = 0;
  for (MachineOperand &UseMO : MRI.use_operands(Reg)) {
    MachineInstr *UseMI = UseMO.getParent();
    bool UseNeedsAGPR = static_cast<const SIInstrInfo *>(TII)->isMAI(*UseMI);
    if (UseNeedsAGPR) {
      HasAGPRUse = true;
      AGPRUseCount++;
    } else {
      HasVGPRUse = true;
      VGPRUseCount++;
    }
    LLVM_DEBUG(dbgs() << "    Use in " << UseMI->getOpcode() << " ("
                      << (UseNeedsAGPR ? "AGPR" : "VGPR") << ")\n");
  }
  LLVM_DEBUG(dbgs() << "  Total uses: " << AGPRUseCount << " AGPR, "
                    << VGPRUseCount << " VGPR\n");

  bool HasAGPRDef = false;
  bool HasVGPRDef = false;
  for (auto &[VNI, IsAGPR] : VNIClassification) {
    if (IsAGPR)
      HasAGPRDef = true;
    else
      HasVGPRDef = true;
  }

  // Partition needed if we have register class mismatches: either we have
  // AGPR-classified defs and VGPR-requiring uses, or VGPR-classified defs and
  // AGPR-requiring uses. The actual def-use connections and copy placement are
  // determined later by connection point analysis.
  bool NeedsPartition =
      (HasAGPRDef && HasVGPRUse) || (HasVGPRDef && HasAGPRUse);

  LLVM_DEBUG(dbgs() << "  Partition decision: HasAGPRDef=" << HasAGPRDef
                    << ", HasVGPRDef=" << HasVGPRDef << ", HasAGPRUse="
                    << HasAGPRUse << ", HasVGPRUse=" << HasVGPRUse
                    << " -> NeedsPartition=" << NeedsPartition << "\n");

  if (!NeedsPartition) {
    LLVM_DEBUG(dbgs() << "  No partition needed - will handle with simple "
                         "register class conversion\n");
    // No partition needed. The live range is homogeneous (all AGPR or all
    // VGPR). For all-AGPR live ranges, the register class will be changed to
    // AGPR later. For all-VGPR live ranges, the register stays VGPR (no rewrite
    // needed).
    return std::nullopt;
  }

  // Partitioning is needed: we have mixed AGPR/VGPR contexts for this register.
  // Strategy: always reuse original as VGPR (matching old implementation) and
  // create a new AGPR temp.
  const TargetRegisterClass *OrigRC = MRI.getRegClass(Reg);
  const TargetRegisterClass *AGPRRC =
      static_cast<const SIRegisterInfo *>(TRI)->getEquivalentAGPRClass(OrigRC);

  // Always reuse original as VGPR.
  Register VGPRReg = Reg;

  // Create AGPR register for the partition.
  // We reach here only if NeedsPartition is true, which requires:
  //   (HasAGPRDef && HasVGPRUse) || (HasVGPRDef && HasAGPRUse)
  // Therefore (HasAGPRDef || HasAGPRUse) is always true at this point.
  assert((HasAGPRDef || HasAGPRUse) &&
         "Should always have AGPR def or use if partitioning");
  Register AGPRReg = MRI.createVirtualRegister(AGPRRC);
  LLVM_DEBUG(dbgs() << "  Reusing original as VGPR, created AGPR temp: "
                    << printReg(AGPRReg, TRI) << "\n");

  LiveRangePartition Partition(Reg, AGPRReg, VGPRReg);
  Partition.IsAGPR = std::move(VNIClassification);

  LLVM_DEBUG(dumpPartition(Partition, TRI));
  unsigned NewVirtRegs = AGPRReg.isValid() ? 1 : 0;
  LLVM_DEBUG(dbgs() << "  Partition created, new virtual registers: "
                    << NewVirtRegs << "\n");

  return Partition;
}

/// Find all connection points where copies must be inserted.
///
/// A connection point occurs when a def from one partition reaches a use
/// in the other partition.
static SmallVector<ConnectionPoint>
findConnectionPoints(const LiveRangePartition &Partition, LiveIntervals *LIS,
                     const TargetInstrInfo *TII,
                     const TargetRegisterInfo *TRI) {

  SmallVector<ConnectionPoint> Points;
  LiveInterval &OrigLI = LIS->getInterval(Partition.OrigReg);

  LLVM_DEBUG(dbgs() << "Finding connection points for "
                    << printReg(Partition.OrigReg, TRI) << "\n");

  // Check all uses of the original register.
  // TODO: these variables can be eliminated when integrating this directly
  // into the scheduler pass as the information is available to that class.
  MachineInstr *FirstInstr =
      LIS->getInstructionFromIndex(OrigLI.begin()->start);
  MachineBasicBlock *FirstMBB = FirstInstr->getParent();
  MachineFunction *MF = FirstMBB->getParent();
  MachineRegisterInfo &MRI = MF->getRegInfo();

  for (MachineOperand &UseMO : MRI.use_operands(Partition.OrigReg)) {
    MachineInstr *UseMI = UseMO.getParent();
    SlotIndex UseIdx = LIS->getInstructionIndex(*UseMI);

    // Find which VNInfo reaches this use.
    const VNInfo *ReachingVNI = OrigLI.getVNInfoAt(UseIdx);
    assert(ReachingVNI && "Use of register must have a reaching VNInfo");

    // Determine if use needs AGPR or VGPR
    bool UseNeedsAGPR = static_cast<const SIInstrInfo *>(TII)->isMAI(*UseMI);

    // Check if reaching VNI is classified as AGPR or VGPR.
    // Note: If ReachingVNI is a PHI, it's classified as AGPR if ANY of its
    // incoming values is an MFMA def (see classifyVNInfo). This means some
    // reaching defs might be VGPR even when the PHI is classified AGPR.
    // If the use needs VGPR and the PHI is AGPR (or vice versa), we handle it
    // below using findReachingDefs() to insert copies on PHI edges. If both the
    // use and PHI are AGPR but the PHI has VGPR reaching defs, those edge
    // copies are handled by a separate loop later that processes all AGPR PHIs.
    auto It = Partition.IsAGPR.find(ReachingVNI);
    assert(It != Partition.IsAGPR.end() && "VNI not in partition");
    bool ReachingVNIIsAGPR = It->second;

    LLVM_DEBUG(dbgs() << "  Use at " << UseIdx << " in " << UseMI->getOpcode()
                      << " (";
               UseMI->print(dbgs(), /*IsStandalone=*/false,
                            /*SkipOpers=*/true, /*SkipDebugLoc=*/true,
                            /*AddNewLine=*/false);
               dbgs() << "): needs " << (UseNeedsAGPR ? "AGPR" : "VGPR")
                      << ", reached by "
                      << (ReachingVNIIsAGPR ? "AGPR" : "VGPR") << " VNI "
                      << ReachingVNI->id << " @ " << ReachingVNI->def << "\n");

    if (UseNeedsAGPR != ReachingVNIIsAGPR) {
      // Connection point detected!
      LLVM_DEBUG(dbgs() << "  ★ CONNECTION POINT DETECTED at " << UseIdx
                        << "\n");
      if (UseNeedsAGPR && !ReachingVNIIsAGPR) {
        // VGPR→AGPR: place copies near reaching defs.
        assert(Partition.AGPRReg.isValid() &&
               "VGPR→AGPR copy but no AGPR register");

        LLVM_DEBUG(dbgs() << "  VGPR→AGPR copy needed\n");

        // Find all non-PHI reaching defs.
        SmallVector<SlotIndex, 8> ReachingDefIdxs;
        findReachingDefs(ReachingVNI, OrigLI, LIS, ReachingDefIdxs);

        LLVM_DEBUG(dbgs() << "  Found " << ReachingDefIdxs.size()
                          << " reaching defs\n");

        // Insert a copy after each reaching def, but only for VGPR defs.
        // MFMA defs are already in AGPR partition and don't need VGPR→AGPR
        // copies.
        for (SlotIndex RDIdx : ReachingDefIdxs) {
          // Check if this reaching def is in the VGPR partition.
          const VNInfo *RDVNI = OrigLI.getVNInfoAt(RDIdx);
          if (!RDVNI) {
            LLVM_DEBUG(dbgs()
                       << "  -> No VNInfo at reaching def " << RDIdx << "\n");
            continue;
          }

          MachineInstr *RDInstr = LIS->getInstructionFromIndex(RDIdx);
          bool IsMAI =
              RDInstr && static_cast<const SIInstrInfo *>(TII)->isMAI(*RDInstr);

          auto RDIt = Partition.IsAGPR.find(RDVNI);
          if (RDIt == Partition.IsAGPR.end()) {
            LLVM_DEBUG(dbgs() << "  -> VNI " << RDVNI->id << " @ " << RDIdx
                              << " not found in partition map\n");
            continue;
          }

          // Skip AGPR defs (e.g., MFMA defs) - they don't need VGPR→AGPR
          // copies.
          if (RDIt->second) {
            LLVM_DEBUG(dbgs() << "  -> Skipping AGPR reaching def VNI "
                              << RDVNI->id << " @ " << RDIdx
                              << " (already in AGPR partition, isMAI=" << IsMAI
                              << ")\n");
            continue;
          }

          LLVM_DEBUG(dbgs()
                     << "  -> VGPR reaching def VNI " << RDVNI->id << " @ "
                     << RDIdx << " needs copy (isMAI=" << IsMAI << ")\n");

          MachineInstr *RD = LIS->getInstructionFromIndex(RDIdx);
          if (RD) {
            Points.emplace_back(RDIdx, Partition.VGPRReg, Partition.AGPRReg,
                                /*IsDefSide=*/true, RD);
            LLVM_DEBUG(dbgs()
                       << "  -> VGPR→AGPR copy needed after reaching def at "
                       << RDIdx << "\n");
          }
        }

        // If no reaching defs found, fall back to placing near use.
        if (ReachingDefIdxs.empty()) {
          Points.emplace_back(UseIdx, Partition.VGPRReg, Partition.AGPRReg,
                              /*IsDefSide=*/false, UseMI);
          LLVM_DEBUG(dbgs() << "  -> VGPR→AGPR copy needed at use " << UseIdx
                            << " (no reaching defs found)\n");
        }
      } else {
        // AGPR→VGPR: place near use.
        assert(Partition.VGPRReg.isValid() &&
               "AGPR→VGPR copy but no VGPR register");

        LLVM_DEBUG(dbgs() << "  ★★★ AGPR→VGPR copy needed at " << UseIdx
                          << " (this is what we're missing!)\n");

        Points.emplace_back(UseIdx, Partition.AGPRReg, Partition.VGPRReg,
                            /*IsDefSide=*/false, UseMI);

        LLVM_DEBUG(
            dbgs() << "  -> AGPR→VGPR copy added to connection points\n");
      }
    }
  }

  // Handle AGPR PHIs with VGPR reaching defs - need VGPR→AGPR copies on those
  // edges
  for (const auto &[VNI, IsAGPR] : Partition.IsAGPR) {
    if (!VNI || !VNI->isPHIDef() || !IsAGPR)
      continue;

    SmallVector<SlotIndex, 8> ReachingDefIdxs;
    findReachingDefs(VNI, OrigLI, LIS, ReachingDefIdxs);

    for (SlotIndex RDIdx : ReachingDefIdxs) {
      const VNInfo *RDVNI = OrigLI.getVNInfoAt(RDIdx);
      if (!RDVNI)
        continue;

      auto RDIt = Partition.IsAGPR.find(RDVNI);
      if (RDIt == Partition.IsAGPR.end())
        continue;

      // If reaching def is VGPR but PHI is AGPR, need VGPR→AGPR copy
      if (!RDIt->second) {
        MachineInstr *RD = LIS->getInstructionFromIndex(RDIdx);
        if (!RD)
          continue;

        Points.emplace_back(RDIdx, Partition.VGPRReg, Partition.AGPRReg,
                            /*IsDefSide=*/true, RD);
      }
    }
  }

  // Deduplicate.
  deduplicateConnectionPoints(Points);

  LLVM_DEBUG({
    dbgs() << "Found " << Points.size() << " connection points:\n";
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

/// Collect all uses of a register BEFORE any copies are inserted.
static SmallVector<MachineOperand *, 8> collectUses(Register Reg,
                                                    MachineRegisterInfo &MRI) {
  SmallVector<MachineOperand *, 8> Uses;

  for (MachineOperand &UseMO : MRI.use_operands(Reg))
    Uses.push_back(&UseMO);

  return Uses;
}

/// Update the collected uses to use the appropriate partition register.
/// For MFMA instructions (which have been rewritten to AGPR form),
/// always use AGPR. For non-MFMA instructions, use VGPR.
static void updateUses(const LiveRangePartition &Partition,
                       ArrayRef<MachineOperand *> Uses,
                       MachineRegisterInfo &MRI, const TargetInstrInfo *TII) {

  LLVM_DEBUG(dbgs() << "Updating uses for "
                    << printReg(Partition.OrigReg, MRI.getTargetRegisterInfo())
                    << "\n");

  // Update all uses.
  for (MachineOperand *UseMO : Uses) {
    MachineInstr *UseMI = UseMO->getParent();

    // MFMA instructions (already rewritten to AGPR form) need AGPR
    bool UseNeedsAGPR = static_cast<const SIInstrInfo *>(TII)->isMAI(*UseMI);
    Register NewReg;
    if (UseNeedsAGPR) {
      assert(Partition.AGPRReg.isValid() &&
             "AGPR use but no AGPR register created");
      NewReg = Partition.AGPRReg;
    } else {
      NewReg = Partition.VGPRReg;
    }

    UseMO->setReg(NewReg);

    LLVM_DEBUG(dbgs() << "  Use in " << (UseNeedsAGPR ? "MFMA" : "non-MFMA")
                      << " -> " << printReg(NewReg, MRI.getTargetRegisterInfo())
                      << "\n");
  }
}

/// Update defs based on VNInfo def points, not raw def operands.
/// This ensures we only update the "primary" def instruction for each VNInfo,
/// avoiding duplicate subreg build sequences.
static void updateDefs(const LiveRangePartition &Partition, LiveIntervals *LIS,
                       MachineRegisterInfo &MRI, const TargetInstrInfo *TII) {

  LLVM_DEBUG(dbgs() << "Updating defs for "
                    << printReg(Partition.OrigReg, MRI.getTargetRegisterInfo())
                    << "\n");

  LiveInterval &LI = LIS->getInterval(Partition.OrigReg);

  // Iterate over each VNInfo and update the def at its def point
  for (VNInfo *VNI : LI.valnos) {
    if (!VNI)
      continue;

    // Skip PHI defs - they don't have a defining instruction
    if (VNI->isPHIDef()) {
      LLVM_DEBUG(dbgs() << "  VNI " << VNI->id << " is PHI, skipping\n");
      continue;
    }

    MachineInstr *DefMI = LIS->getInstructionFromIndex(VNI->def);
    if (!DefMI) {
      LLVM_DEBUG(dbgs() << "  VNI " << VNI->id << " has no def instruction\n");
      continue;
    }

    // Determine which partition this VNI belongs to
    auto It = Partition.IsAGPR.find(VNI);
    if (It == Partition.IsAGPR.end()) {
      LLVM_DEBUG(dbgs() << "  VNI " << VNI->id << " not in partition map\n");
      continue;
    }

    bool MFMADefsAreAGPR = It->second;
    Register NewReg;
    if (MFMADefsAreAGPR) {
      assert(Partition.AGPRReg.isValid() &&
             "AGPR def but no AGPR register created");
      NewReg = Partition.AGPRReg;
    } else {
      NewReg = Partition.VGPRReg;
    }

    // Update all defs of OrigReg in this instruction to use NewReg
    for (MachineOperand &MO : DefMI->operands()) {
      if (MO.isReg() && MO.isDef() && MO.getReg() == Partition.OrigReg) {
        MO.setReg(NewReg);
        LLVM_DEBUG(dbgs() << "  Updated def in VNI " << VNI->id << " @ "
                          << VNI->def << " to "
                          << printReg(NewReg, MRI.getTargetRegisterInfo())
                          << "\n");
      }
    }
  }
}

} // anonymous namespace

//===----------------------------------------------------------------------===//
// Public Interface (for integration with RewriteMFMAFormStage)
//===----------------------------------------------------------------------===//

namespace llvm {

/// MFMA rewrite using live range partitioning.
///
/// This is the entry point that replaces RewriteMFMAFormStage::rewrite().
bool rewriteWithPartitioning(
    ArrayRef<std::pair<MachineInstr *, unsigned>> RewriteCands,
    MachineFunction &MF, LiveIntervals *LIS, const TargetInstrInfo *TII,
    const TargetRegisterInfo *TRI, const SIInstrInfo *SII) {

  MachineRegisterInfo &MRI = MF.getRegInfo();

  LLVM_DEBUG(dbgs() << "=== Starting live range partitioning rewrite ===\n");
  LLVM_DEBUG(dbgs() << "Function: " << MF.getName() << "\n");
  LLVM_DEBUG(dbgs() << "Processing " << RewriteCands.size() << " candidates\n");

  // Step 1: Collect all unique registers that participate in MFMAs.
  // Track the roles each register plays using flags.
  enum RegRole : unsigned {
    MFMA_DST = 1 << 0,  // Register is defined by MFMA (destination operand)
    MFMA_SRC2 = 1 << 1, // Register is used as src2 in MFMA (accumulator input)
  };

  struct RegInfo {
    unsigned Roles = 0; // Bitmask of RegRole flags
    SmallVector<MachineOperand *, 8> Uses;
  };
  DenseMap<Register, RegInfo> RegistersToPartition;

  for (auto &[MI, OrigOpcode] : RewriteCands) {
    int ReplacementOp = AMDGPU::getMFMASrcCVDstAGPROp(MI->getOpcode());
    if (ReplacementOp == -1)
      continue;

    // Track dst register - mark as MFMA_DST.
    // A register can be both dst and src2 (accumulator chains), so we OR the
    // flags.
    Register DstReg = MI->getOperand(0).getReg();
    if (DstReg.isVirtual()) {
      auto &Info = RegistersToPartition[DstReg];
      Info.Roles |= MFMA_DST;
      if (Info.Uses.empty()) {
        // First time seeing this register - collect its uses.
        Info.Uses = collectUses(DstReg, MRI);
      }
    }

    // Track src2 register - mark as MFMA_SRC2.
    MachineOperand *Src2 = SII->getNamedOperand(*MI, AMDGPU::OpName::src2);
    if (Src2 && Src2->isReg()) {
      Register Src2Reg = Src2->getReg();
      if (Src2Reg.isVirtual()) {
        auto &Info = RegistersToPartition[Src2Reg];
        Info.Roles |= MFMA_SRC2;
        if (Info.Uses.empty()) {
          // First time seeing this register - collect its uses.
          Info.Uses = collectUses(Src2Reg, MRI);
        }
      }
    }
  }

  // Step 2: Partition each unique register once.
  DenseMap<Register, LiveRangePartition> Partitions;
  for (auto &[Reg, Info] : RegistersToPartition) {
    bool MFMADefsAreAGPR = (Info.Roles & MFMA_DST) != 0;

    LLVM_DEBUG({
      dbgs() << "\nPartitioning " << printReg(Reg, TRI) << " (";
      if (Info.Roles & MFMA_DST)
        dbgs() << "MFMA_DST ";
      if (Info.Roles & MFMA_SRC2)
        dbgs() << "MFMA_SRC2";
      dbgs() << ")\n";
    });

    auto Partition =
        partitionLiveRange(Reg, MFMADefsAreAGPR, LIS, MRI, TII, TRI);
    if (Partition) {
      Partitions[Reg] = *Partition;
      LLVM_DEBUG(dbgs() << "  ✓ Partition created for " << printReg(Reg, TRI)
                        << "\n");
    } else {
      LLVM_DEBUG(dbgs() << "  ✗ NO PARTITION created for " << printReg(Reg, TRI)
                        << " (returned nullopt)\n");
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

    // Handle destination operand.
    Register DstReg = MI->getOperand(0).getReg();
    if (!DstReg.isVirtual()) {
      LLVM_DEBUG(dbgs() << "Aborting: dst is physical register\n");
      return false;
    }

    // Use the pre-computed partition for dst.
    auto DstIt = Partitions.find(DstReg);
    if (DstIt == Partitions.end()) {
      LLVM_DEBUG(dbgs() << "No partition for dst\n");
      continue;
    }

    SmallVector<ConnectionPoint> DstConnections =
        findConnectionPoints(DstIt->second, LIS, TII, TRI);

    // Accumulate connection points, don't insert yet.
    AllConnectionPoints.append(DstConnections.begin(), DstConnections.end());

    // Handle src2 operand.
    MachineOperand *Src2 = SII->getNamedOperand(*MI, AMDGPU::OpName::src2);
    if (Src2 && Src2->isReg()) {
      Register Src2Reg = Src2->getReg();
      if (!Src2Reg.isVirtual()) {
        LLVM_DEBUG(dbgs() << "Aborting: src2 is physical register\n");
        return false;
      }

      // Use the pre-computed partition for src2.
      auto Src2It = Partitions.find(Src2Reg);
      if (Src2It == Partitions.end()) {
        LLVM_DEBUG(dbgs() << "No partition for src2\n");
        continue;
      }

      auto Src2Connections =
          findConnectionPoints(Src2It->second, LIS, TII, TRI);

      // Accumulate connection points, don't insert yet.
      AllConnectionPoints.append(Src2Connections.begin(),
                                 Src2Connections.end());
    }

    Rewritten++;
  }

  // Step 4: Optimize copy placement (group copies in same block)
  LLVM_DEBUG(dbgs() << "\n--- Optimizing copy placement ---\n");
  optimizeCopyPlacement(AllConnectionPoints, LIS);

  // TODO: at this point, a cost analysis can be inserted that walks the
  // optimized copy descriptors (connection points) and calculate the cost
  // of each copy. This could  then replace the copy cost analysis portion
  // of getRewriteCost().

  // Step 5: Change opcodes and update all operands (before any copies exist in
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

  // For registers that don't need partitioning, just change their class to
  // AGPR. These are registers with only AGPR uses (or only VGPR uses, which we
  // skip).
  LLVM_DEBUG(dbgs() << "\n--- Converting non-partitioned registers to AGPR "
                       "---\n");
  for (auto &[Reg, Info] : RegistersToPartition) {
    // Skip if we already created a partition for this register
    if (Partitions.count(Reg))
      continue;

    // Check if this register needs AGPR class by examining its uses
    bool NeedsAGPR = false;
    for (MachineOperand *UseMO : Info.Uses) {
      MachineInstr *UseMI = UseMO->getParent();
      if (static_cast<const SIInstrInfo *>(TII)->isMAI(*UseMI)) {
        NeedsAGPR = true;
        break;
      }
    }

    // If needs AGPR, convert to AGPR class
    if (NeedsAGPR) {
      const TargetRegisterClass *OrigRC = MRI.getRegClass(Reg);
      const TargetRegisterClass *AGPRRC =
          static_cast<const SIRegisterInfo *>(TRI)->getEquivalentAGPRClass(
              OrigRC);
      MRI.setRegClass(Reg, AGPRRC);

      LLVM_DEBUG(dbgs() << "  Converted " << printReg(Reg, TRI)
                        << " to AGPR (no partition needed)\n");
    }
  }

  // Then, update uses and defs to use partitioned registers.
  // Uses are updated from the collected list, defs are updated via VNInfo.
  for (auto &[Reg, Partition] : Partitions) {
    auto It = RegistersToPartition.find(Reg);
    if (It != RegistersToPartition.end()) {
      updateUses(Partition, It->second.Uses, MRI, TII);
      updateDefs(Partition, LIS, MRI, TII);
    }
  }

  // Finally, change the register class of the original registers.
  // This must be done after all operands have been updated.
  LLVM_DEBUG(dbgs() << "\n--- Changing register classes ---\n");
  for (auto &[Reg, Partition] : Partitions) {
    const TargetRegisterClass *OrigRC = MRI.getRegClass(Reg);

    if (Partition.AGPRReg == Reg) {
      // Original is used as AGPR - change to AGPR class
      const TargetRegisterClass *AGPRRC =
          static_cast<const SIRegisterInfo *>(TRI)->getEquivalentAGPRClass(
              OrigRC);
      MRI.setRegClass(Reg, AGPRRC);
      LLVM_DEBUG(dbgs() << "  Changed " << printReg(Reg, TRI)
                        << " to AGPR class\n");
    } else {
      // Original is reused as VGPR partition - verify it's still VGPR class
      assert(Partition.VGPRReg == Reg &&
             "Expected VGPRReg to be original register");
      assert(MRI.getRegClass(Reg) == static_cast<const SIRegisterInfo *>(TRI)
                                         ->getEquivalentVGPRClass(OrigRC) &&
             "Original register changed from VGPR class");
    }
  }

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

  // If we made any changes (partitions created, register classes converted,
  // or opcodes changed), we need to reanalyze and return true.
  bool MadeChanges = Rewritten > 0 || !RewriteCands.empty();

  if (MadeChanges) {
    // Reanalyze all live intervals.
    LLVM_DEBUG(dbgs() << "Reanalyzing LiveIntervals\n");
    LIS->reanalyze(MF);
    return true;
  }

  return false;
}

} // namespace llvm
