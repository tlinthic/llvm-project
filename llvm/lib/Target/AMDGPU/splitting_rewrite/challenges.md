# Implementation Challenges and Solutions

## Challenge 1: PHI Node Handling

### Problem
PHI nodes can merge values from different partitions. For example:
```
BB1:
  %0 = MFMA ...           ; AGPR def

BB2:  
  %0 = NON_MFMA ...       ; VGPR def

BB3:
  %1 = PHI %0(BB1), %0(BB2)
  use %1 in MFMA          ; Needs AGPR
```

The PHI merges an AGPR value and a VGPR value but the use requires AGPR.

### Solution Options

**Option 1: Split the PHI**
Create separate PHI nodes for each partition:
```
BB3:
  %1_agpr = PHI %0_agpr(BB1), undef(BB2)
  %1_vgpr = PHI undef(BB1), %0_vgpr(BB2)
  %1_agpr_final = COPY %1_vgpr  ; if coming from BB2
```

**Option 2: Insert copies in predecessors**
```
BB1:
  %0_agpr = MFMA ...
  ; no copy needed

BB2:
  %0_vgpr = NON_MFMA ...
  %0_agpr = COPY %0_vgpr  ; Insert before branch

BB3:
  %1 = PHI %0_agpr(BB1), %0_agpr(BB2)
  use %1 in MFMA
```

**Recommendation:** Option 2 is cleaner and maintains SSA form more naturally. The interference detection should identify that the VGPR incoming value needs a copy to AGPR.

### Implementation
```cpp
void classifyPHIDef(VNInfo *PhiVNI, LiveRangePartition &Partition) {
  // Analyze uses to determine which partition this PHI should belong to
  bool HasAGPRUse = false;
  bool HasVGPRUse = false;
  
  // Scan uses of this VNI
  for (uses of this PHI value) {
    if (isAGPRUse) HasAGPRUse = true;
    else HasVGPRUse = true;
  }
  
  // If all uses are same class, assign PHI to that partition
  // The incoming values from wrong partition will trigger interference
  if (HasAGPRUse && !HasVGPRUse) {
    Partition.AGPRVNMap[PhiVNI] = ...;
  } else if (HasVGPRUse && !HasAGPRUse) {
    Partition.VGPRVNMap[PhiVNI] = ...;
  } else {
    // Mixed uses - need to handle carefully
    // Conservative: treat as VGPR and let interferences handle copies
    Partition.VGPRVNMap[PhiVNI] = ...;
  }
}
```

## Challenge 2: Subregister Lanes

### Problem
Vector registers can have partial writes to different lanes:
```
%0:sub0 = MFMA ...      ; Write low 32 bits with AGPR
%0:sub1 = NON_MFMA ...  ; Write high 32 bits with VGPR
use %0 in MFMA          ; Use full 64 bits
```

Different lanes might need different register classes.

### Solution
Use `LiveInterval::SubRange` to track lanes independently:

```cpp
LiveInterval &LI = LIS->getInterval(Reg);

// Handle main range
partitionMainRange(LI, Partition);

// Handle each subrange independently
for (LiveInterval::SubRange &SR : LI.subranges()) {
  LaneBitmask Lanes = SR.LaneMask;
  
  // Partition this subrange
  for (VNInfo *VNI : SR.valnos) {
    if (needsAGPR(VNI, Lanes)) {
      // This lane's VNI goes to AGPR partition
    } else {
      // This lane's VNI goes to VGPR partition
    }
  }
}
```

### Complexity Mitigation
The current `hasSafePartialRedefs()` check already rejects unsafe patterns. We can initially:
1. Keep the same safety check
2. Only handle whole-register splits
3. Add subregister support later if needed

## Challenge 3: Copy Deduplication

### Problem
Multiple uses might require the same copy:
```
%0_vgpr = NON_MFMA ...

BB:
  use %0_vgpr in MFMA_1   ; Needs VGPR→AGPR copy
  use %0_vgpr in MFMA_2   ; Needs same copy
```

We should only insert one copy.

### Solution
Deduplicate interference points by location and direction:

```cpp
void deduplicateConnectionPoints(SmallVectorImpl<ConnectionPoint> &Points) {
  // Sort by location and registers
  llvm::sort(Points, [](const ConnectionPoint &A, const ConnectionPoint &B) {
    if (A.Location != B.Location)
      return A.Location < B.Location;
    if (A.SrcReg != B.SrcReg)
      return A.SrcReg < B.SrcReg;
    return A.DstReg < B.DstReg;
  });
  
  // Remove duplicates
  Points.erase(llvm::unique(Points, [](const ConnectionPoint &A, 
                                       const ConnectionPoint &B) {
    return A.Location == B.Location && 
           A.SrcReg == B.SrcReg && 
           A.DstReg == B.DstReg;
  }), Points.end());
}
```

## Challenge 4: Cross-Block Copy Placement

### Problem
When a def in one block reaches uses in multiple successor blocks:
```
BB1:
  %0_vgpr = NON_MFMA ...

BB2:                     BB3:
  use %0_vgpr in MFMA      use %0_vgpr in NON_MFMA
```

BB2 needs VGPR→AGPR copy but BB3 doesn't.

### Solution
The interference detection naturally handles this:
- The use in BB2 triggers an interference point
- The use in BB3 doesn't (same partition)
- Copy is inserted at the beginning of BB2

For the "near def" strategy (VGPR→AGPR), we might want to hoist:
```cpp
if (CopyNearDef && allSuccessorsNeedCopy(DefVNI)) {
  // Insert copy after def in BB1
  InsertPt = after def in BB1;
} else {
  // Insert copy at beginning of each successor that needs it
  for (each successor needing copy) {
    InsertPt = beginning of successor;
  }
}
```

But this optimization can come later. Initial implementation can just place at use points.

## Challenge 5: Tied Operands

### Problem
Some MFMA instructions have tied operands (e.g., accumulator):
```
%0 = MFMA %1, %2, %0  ; dst tied to src3
```

Both the def and use of %0 need to be in the same partition.

### Solution
When partitioning, if an instruction has a tied operand:
```cpp
if (MI->getOperand(DefIdx).isTied()) {
  unsigned UseIdx = MI->findTiedOperandIdx(DefIdx);
  // Both def and use must be in same partition
  // Use the def's partition classification
}
```

This is similar to handling in current implementation.

## Challenge 6: LiveInterval Consistency

### Problem
After splitting, we need to ensure LiveIntervals are consistent:
- No overlapping ranges for the original register
- Proper SSA form maintained
- VNInfo properly updated

### Solution
Use `LiveRangeEdit` or manually manage:

```cpp
// Option 1: Use LiveRangeEdit (safer but more complex)
LiveRangeEdit LRE(&OrigLI, NewVRegs, MF, *LIS, VRM);
LRE.split(...);

// Option 2: Manual (more control)
// 1. Create new LiveIntervals for new registers
LiveInterval &AGPRLI = LIS->createEmptyInterval(Partition.AGPRReg);
LiveInterval &VGPRLI = LIS->createEmptyInterval(Partition.VGPRReg);

// 2. Copy segments
for (auto &Seg : Partition.AGPRSegments)
  AGPRLI.addSegment(Seg);
for (auto &Seg : Partition.VGPRSegments)
  VGPRLI.addSegment(Seg);

// 3. Remove original interval
LIS->removeInterval(Partition.OrigReg);

// 4. OR just reanalyze everything at the end
LIS->reanalyze(MF);
```

**Recommendation:** For first implementation, use `LIS->reanalyze(MF)` at the end. It's less efficient but guaranteed correct.

## Challenge 7: Interaction with Cost Model

### Problem
The current implementation does speculative rewriting to calculate accurate register pressure. With live range splitting, this is more complex.

### Solution
Keep the two-phase approach:
1. `initHeuristics()`: Do speculative split (just partition, don't create registers)
2. `getRewriteCost()`: Calculate RP with speculated partitions
3. `rewrite()`: Actually perform the split if cost is good

The partition structure can be created without actually modifying the MIR:

```cpp
// In initHeuristics - just classify, don't modify MIR
std::vector<LiveRangePartition> SpeculativePartitions;
for (each candidate) {
  auto Partition = analyzeLiveRange(Reg); // read-only
  SpeculativePartitions.push_back(Partition);
}

// Calculate RP with temporary register class changes
// (similar to current approach)

// In rewrite - actually perform the split
for (auto &Partition : SpeculativePartitions) {
  createNewRegisters(Partition);
  insertCopies(Partition);
  updateOperands(Partition);
}
```

## Challenge 8: Debugging

### Problem
When things go wrong, need to understand which partition a value belongs to.

### Solution
Add comprehensive debug output:

```cpp
LLVM_DEBUG({
  dbgs() << "Partitioning " << printReg(Partition.OrigReg, TRI) << ":\n";
  dbgs() << "  AGPR segments: ";
  for (auto &Seg : Partition.AGPRSegments)
    dbgs() << Seg << " ";
  dbgs() << "\n  VGPR segments: ";
  for (auto &Seg : Partition.VGPRSegments)
    dbgs() << Seg << " ";
  dbgs() << "\nInterference points:\n";
  for (auto &IP : Interferences)
    dbgs() << "  " << IP.Location << ": " 
           << printReg(IP.SrcReg, TRI) << " -> " 
           << printReg(IP.DstReg, TRI) << "\n";
});
```

Also add MIR verification after each major step.
