# Detailed Design: Live Range Splitting Rewrite

## High-Level Algorithm

```
For each MFMA rewrite candidate:
  1. Partition the live interval into AGPR and VGPR segments
  2. Create two new virtual registers (one AGPR, one VGPR)
  3. Identify interference points (where AGPR LR flows into VGPR LR or vice versa)
  4. Insert copies at interference points
  5. Update all operands to use the correct register
  6. Update LiveIntervals
```

## Core Data Structures

### LiveInterval Partition

```cpp
struct LiveRangePartition {
  Register OrigReg;
  Register AGPRReg;  // New register for AGPR segments
  Register VGPRReg;  // New register for VGPR segments
  
  // Segments belonging to each partition
  SmallVector<LiveInterval::Segment, 4> AGPRSegments;
  SmallVector<LiveInterval::Segment, 4> VGPRSegments;
  
  // VNInfo mappings: OrigVNI -> NewVNI for each partition
  DenseMap<VNInfo*, VNInfo*> AGPRVNMap;
  DenseMap<VNInfo*, VNInfo*> VGPRVNMap;
};
```

### Interference Point

```cpp
struct ConnectionPoint {
  SlotIndex Location;
  Register SrcReg;   // Source of the copy
  Register DstReg;   // Destination of the copy
  bool IsDefSide;    // true = insert after def, false = insert before use
  
  // For region boundary tracking
  MachineInstr *InsertNear;
  unsigned RegionIdx;
};
```

## Step 1: Partition Live Interval

For a register being rewritten:

```cpp
LiveRangePartition partitionLiveRange(Register Reg, bool DefIsAGPR) {
  LiveInterval &LI = LIS->getInterval(Reg);
  LiveRangePartition Partition;
  Partition.OrigReg = Reg;
  
  // Create new registers
  const TargetRegisterClass *OrigRC = MRI.getRegClass(Reg);
  Partition.AGPRReg = MRI.createVirtualRegister(SRI->getEquivalentAGPRClass(OrigRC));
  Partition.VGPRReg = MRI.createVirtualRegister(SRI->getEquivalentVGPRClass(OrigRC));
  
  // Classify each VNInfo
  for (VNInfo *VNI : LI.valnos) {
    if (VNI->isPHIDef()) {
      // PHI handling - needs special care
      classifyPHIDef(VNI, Partition);
      continue;
    }
    
    MachineInstr *DefMI = LIS->getInstructionFromIndex(VNI->def);
    bool IsAGPRDef = DefIsAGPR ? TII->isMAI(*DefMI) : false;
    
    if (IsAGPRDef) {
      Partition.AGPRSegments.append(segments for this VNI);
      Partition.AGPRVNMap[VNI] = createNewVNInfo(Partition.AGPRReg, VNI);
    } else {
      Partition.VGPRSegments.append(segments for this VNI);
      Partition.VGPRVNMap[VNI] = createNewVNInfo(Partition.VGPRReg, VNI);
    }
  }
  
  return Partition;
}
```

## Step 2: Find Interference Points

An interference occurs when:
- A def from one partition reaches a use in the other partition
- Control flow merges values from different partitions (PHI)

```cpp
SmallVector<ConnectionPoint> findConnectionPoints(
    const LiveRangePartition &Partition,
    bool CopyNearDef) {
  
  SmallVector<ConnectionPoint> Points;
  
  // For each use of the original register
  for (MachineOperand &UseMO : MRI.use_operands(Partition.OrigReg)) {
    MachineInstr *UseMI = UseMO.getParent();
    SlotIndex UseIdx = LIS->getInstructionIndex(*UseMI);
    
    // Find which VNInfo reaches this use
    LiveInterval &OrigLI = LIS->getInterval(Partition.OrigReg);
    VNInfo *ReachingVNI = OrigLI.getVNInfoAt(UseIdx);
    
    // Determine if use needs AGPR or VGPR
    bool UseNeedsAGPR = TII->isMAI(*UseMI);
    
    // Check if reaching def is in the wrong partition
    bool DefIsAGPR = Partition.AGPRVNMap.contains(ReachingVNI);
    bool DefIsVGPR = Partition.VGPRVNMap.contains(ReachingVNI);
    
    if (UseNeedsAGPR && DefIsVGPR) {
      // Need VGPR→AGPR copy
      Points.push_back({
        Location: CopyNearDef ? ReachingVNI->def : UseIdx,
        SrcReg: Partition.VGPRReg,
        DstReg: Partition.AGPRReg,
        IsDefSide: CopyNearDef,
        InsertNear: CopyNearDef ? LIS->getInstructionFromIndex(ReachingVNI->def) : UseMI
      });
    } else if (!UseNeedsAGPR && DefIsAGPR) {
      // Need AGPR→VGPR copy
      Points.push_back({
        Location: CopyNearDef ? ReachingVNI->def : UseIdx,
        SrcReg: Partition.AGPRReg,
        DstReg: Partition.VGPRReg,
        IsDefSide: !CopyNearDef,  // For AGPR→VGPR, place near use
        InsertNear: UseMI
      });
    }
  }
  
  // Deduplicate interference points
  deduplicateConnectionPoints(Points);
  
  return Points;
}
```

## Step 3: Insert Copies

```cpp
void insertCopies(ArrayRef<ConnectionPoint> Points,
                  DenseMap<MachineInstr*, unsigned> &FirstMIToRegion,
                  DenseMap<MachineInstr*, unsigned> &LastMIToRegion) {
  
  for (const ConnectionPoint &IP : Points) {
    MachineBasicBlock *MBB = IP.InsertNear->getParent();
    MachineBasicBlock::iterator InsertPt;
    
    if (IP.IsDefSide) {
      // Insert after the def
      InsertPt = std::next(IP.InsertNear->getIterator());
    } else {
      // Insert before the use
      InsertPt = IP.InsertNear->getIterator();
    }
    
    MachineInstrBuilder Copy = 
        BuildMI(*MBB, InsertPt, IP.InsertNear->getDebugLoc(), 
                TII->get(TargetOpcode::COPY))
            .addDef(IP.DstReg)
            .addUse(IP.SrcReg);
    
    LIS->InsertMachineInstrInMaps(*Copy);
    
    // Update region boundaries if needed
    updateRegionBoundaries(Copy, IP, FirstMIToRegion, LastMIToRegion);
  }
}
```

## Step 4: Update Operands

```cpp
void updateOperands(const LiveRangePartition &Partition) {
  LiveInterval &OrigLI = LIS->getInterval(Partition.OrigReg);
  
  // Update all uses
  for (MachineOperand &UseMO : MRI.use_operands(Partition.OrigReg)) {
    MachineInstr *UseMI = UseMO.getParent();
    SlotIndex UseIdx = LIS->getInstructionIndex(*UseMI);
    
    VNInfo *ReachingVNI = OrigLI.getVNInfoAt(UseIdx);
    
    if (Partition.AGPRVNMap.contains(ReachingVNI)) {
      UseMO.setReg(Partition.AGPRReg);
    } else {
      UseMO.setReg(Partition.VGPRReg);
    }
  }
  
  // Update all defs
  for (MachineOperand &DefMO : MRI.def_operands(Partition.OrigReg)) {
    MachineInstr *DefMI = DefMO.getParent();
    SlotIndex DefIdx = LIS->getInstructionIndex(*DefMI);
    
    VNInfo *VNI = OrigLI.getVNInfoAt(DefIdx);
    
    if (Partition.AGPRVNMap.contains(VNI)) {
      DefMO.setReg(Partition.AGPRReg);
    } else {
      DefMO.setReg(Partition.VGPRReg);
    }
  }
}
```

## PHI Handling

PHI nodes are challenging because they merge values from different predecessors:

```cpp
void classifyPHIDef(VNInfo *PhiVNI, LiveRangePartition &Partition) {
  // Strategy 1: Classify based on uses
  // If all uses are AGPR, the PHI value should be AGPR
  // If all uses are VGPR, the PHI value should be VGPR
  // If mixed, we need copies
  
  // Strategy 2: Classify based on incoming values
  // If all incoming values are from the same partition, 
  // the PHI belongs to that partition
  
  // For now, use conservative approach: 
  // PHI values that have mixed uses/defs require explicit handling
}
```

## Subregister Handling

For registers with subregister lanes (e.g., vector registers):

```cpp
// Use LiveInterval::SubRanges
// Each lane can be split independently
for (LiveInterval::SubRange &SR : LI.subranges()) {
  // Partition this subrange based on lane mask
  partitionSubRange(SR, Partition);
}
```

## Region Boundary Updates

When copies are inserted at region boundaries:

```cpp
void updateRegionBoundaries(MachineInstr *Copy, 
                            const ConnectionPoint &IP,
                            DenseMap<MachineInstr*, unsigned> &FirstMIToRegion,
                            DenseMap<MachineInstr*, unsigned> &LastMIToRegion) {
  // If copy inserted after last MI of a region, update region end
  if (LastMIToRegion.contains(IP.InsertNear)) {
    unsigned RegionIdx = LastMIToRegion[IP.InsertNear];
    DAG.Regions[RegionIdx].second = Copy;
    LastMIToRegion.erase(IP.InsertNear);
    LastMIToRegion[Copy] = RegionIdx;
  }
  
  // If copy inserted before first MI of a region, update region start
  if (FirstMIToRegion.contains(IP.InsertNear)) {
    unsigned RegionIdx = FirstMIToRegion[IP.InsertNear];
    DAG.Regions[RegionIdx].first = Copy;
    FirstMIToRegion.erase(IP.InsertNear);
    FirstMIToRegion[Copy] = RegionIdx;
  }
}
```

## Main Rewrite Function

```cpp
bool rewrite(const std::vector<std::pair<MachineInstr *, unsigned>> &RewriteCands) {
  DenseMap<MachineInstr *, unsigned> FirstMIToRegion;
  DenseMap<MachineInstr *, unsigned> LastMIToRegion;
  
  // Build region maps
  for (unsigned Region = 0; Region < DAG.Regions.size(); Region++) {
    RegionBoundaries Entry = DAG.Regions[Region];
    if (Entry.first != Entry.second) {
      FirstMIToRegion[&*Entry.first] = Region;
      if (Entry.second != Entry.first->getParent()->end())
        LastMIToRegion[&*Entry.second] = Region;
    }
  }
  
  // Process each MFMA rewrite
  for (auto &[MI, OrigOpcode] : RewriteCands) {
    int ReplacementOp = AMDGPU::getMFMASrcCVDstAGPROp(MI->getOpcode());
    MI->setDesc(TII->get(ReplacementOp));
    
    // Handle destination (MFMA def -> AGPR)
    Register DstReg = MI->getOperand(0).getReg();
    auto DstPartition = partitionLiveRange(DstReg, /*DefIsAGPR=*/true);
    auto DstInterferences = findConnectionPoints(DstPartition, /*CopyNearDef=*/false);
    insertCopies(DstInterferences, FirstMIToRegion, LastMIToRegion);
    updateOperands(DstPartition);
    
    // Handle src2 (non-MFMA def -> VGPR, MFMA use -> AGPR)
    MachineOperand *Src2 = TII->getNamedOperand(*MI, AMDGPU::OpName::src2);
    if (Src2->isReg()) {
      Register Src2Reg = Src2->getReg();
      auto Src2Partition = partitionLiveRange(Src2Reg, /*DefIsAGPR=*/false);
      auto Src2Interferences = findConnectionPoints(Src2Partition, /*CopyNearDef=*/true);
      insertCopies(Src2Interferences, FirstMIToRegion, LastMIToRegion);
      updateOperands(Src2Partition);
    }
  }
  
  // Reanalyze LiveIntervals
  LIS->reanalyze(MF);
  
  // Update region live-ins
  RegionPressureMap LiveInUpdater(&DAG, false);
  LiveInUpdater.buildLiveRegMap();
  for (unsigned Region = 0; Region < DAG.Regions.size(); Region++)
    DAG.LiveIns[Region] = LiveInUpdater.getLiveRegsForRegionIdx(Region);
  
  return true;
}
```

## Complexity Comparison

**Current implementation:**
- 342 lines
- 7 complex data structures with intricate relationships
- Custom reaching def/use traversal
- 3 separate copy insertion cases handled differently

**Live range splitting approach:**
- ~150-200 lines estimated
- 2 primary data structures (LiveRangePartition, ConnectionPoint)
- Leverages existing LiveInterval infrastructure
- Unified copy insertion logic
