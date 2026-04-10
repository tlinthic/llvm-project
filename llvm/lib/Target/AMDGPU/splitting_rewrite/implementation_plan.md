# Implementation Plan

## Phase 1: Core Infrastructure (Minimal Viable Implementation)

### Goal
Get a working prototype that handles the simple case without PHI nodes or subregisters.

### Tasks

#### 1.1: Define Data Structures
```cpp
// In GCNSchedStrategy.cpp or new splitting_rewrite.cpp

struct LiveRangePartition {
  Register OrigReg;
  Register AGPRReg;
  Register VGPRReg;
  
  // VNInfo mappings
  DenseMap<VNInfo*, bool> IsAGPR;  // true if VNI is AGPR, false if VGPR
};

struct ConnectionPoint {
  SlotIndex Location;
  Register SrcReg;
  Register DstReg;
  bool IsDefSide;
  MachineInstr *InsertNear;
};
```

#### 1.2: Implement Basic Partition Function
```cpp
LiveRangePartition partitionLiveRange(Register Reg, bool DefIsAGPR) {
  // For each VNInfo in the LiveInterval:
  //   - Classify as AGPR or VGPR based on defining instruction
  //   - Skip PHI nodes initially (conservative: abort if PHI found)
  // Return the partition
}
```

**Acceptance criteria:**
- Can classify all non-PHI VNInfos
- Returns nullopt or aborts if PHI encountered

#### 1.3: Implement Interference Detection
```cpp
SmallVector<ConnectionPoint> findConnectionPoints(
    const LiveRangePartition &Partition,
    bool CopyNearDef) {
  // For each use of OrigReg:
  //   - Find reaching VNInfo
  //   - Check if use needs different class than reaching def
  //   - If so, add interference point
  // Deduplicate
}
```

**Acceptance criteria:**
- Correctly identifies all cross-partition uses
- Deduplicates redundant interference points
- Handles CopyNearDef flag correctly

#### 1.4: Implement Copy Insertion
```cpp
void insertCopies(ArrayRef<ConnectionPoint> Points) {
  // For each interference point:
  //   - Build COPY instruction
  //   - Insert at appropriate location
  //   - Call LIS->InsertMachineInstrInMaps
}
```

**Acceptance criteria:**
- Copies inserted at correct locations
- LiveIntervals updated correctly
- No region boundary handling yet (can break regions for now)

#### 1.5: Implement Operand Update
```cpp
void updateOperands(const LiveRangePartition &Partition) {
  // For each use/def of OrigReg:
  //   - Find reaching VNInfo
  //   - Replace with AGPRReg or VGPRReg based on partition
}
```

**Acceptance criteria:**
- All operands updated to reference correct new register
- No dangling references to OrigReg

#### 1.6: Wire into rewrite()
Replace the body of `RewriteMFMAFormStage::rewrite()` with:
```cpp
for (auto &[MI, OrigOpcode] : RewriteCands) {
  MI->setDesc(TII->get(ReplacementOp));
  
  // Dst
  Register DstReg = MI->getOperand(0).getReg();
  if (hasPHI(DstReg)) continue;  // Skip for now
  auto DstPart = partitionLiveRange(DstReg, true);
  auto DstInterf = findConnectionPoints(DstPart, false);
  insertCopies(DstInterf);
  updateOperands(DstPart);
  
  // Src2
  if (Src2->isReg()) {
    Register Src2Reg = Src2->getReg();
    if (hasPHI(Src2Reg)) continue;  // Skip for now
    auto Src2Part = partitionLiveRange(Src2Reg, false);
    auto Src2Interf = findConnectionPoints(Src2Part, true);
    insertCopies(Src2Interf);
    updateOperands(Src2Part);
  }
}

LIS->reanalyze(MF);
// Update region live-ins (copy from current implementation)
```

**Acceptance criteria:**
- Compiles and links
- Passes simple test cases without PHI nodes
- Can skip cases with PHI nodes gracefully

### Testing Phase 1
Create test cases:
1. Simple MFMA with non-MFMA def and use (Example 1)
2. MFMA chain (Example 2)
3. Multiple uses in same block

**Milestone:** Basic functionality working without PHI nodes or complex control flow.

---

## Phase 2: Region Boundary Handling

### Goal
Correctly update region boundaries when copies are inserted.

### Tasks

#### 2.1: Track Region Boundaries
```cpp
// Build FirstMI/LastMI maps (copy from current implementation)
DenseMap<MachineInstr *, unsigned> FirstMIToRegion;
DenseMap<MachineInstr *, unsigned> LastMIToRegion;

for (unsigned Region = 0; Region < DAG.Regions.size(); Region++) {
  RegionBoundaries Entry = DAG.Regions[Region];
  if (Entry.first != Entry.second) {
    FirstMIToRegion[&*Entry.first] = Region;
    if (Entry.second != Entry.first->getParent()->end())
      LastMIToRegion[&*Entry.second] = Region;
  }
}
```

#### 2.2: Update insertCopies to Handle Boundaries
```cpp
void insertCopies(ArrayRef<ConnectionPoint> Points,
                  DenseMap<MachineInstr*, unsigned> &FirstMIToRegion,
                  DenseMap<MachineInstr*, unsigned> &LastMIToRegion) {
  for (const ConnectionPoint &IP : Points) {
    // ... insert copy ...
    
    // Check if we inserted at a region boundary
    if (IP.IsDefSide && LastMIToRegion.contains(IP.InsertNear)) {
      unsigned RegionIdx = LastMIToRegion[IP.InsertNear];
      DAG.Regions[RegionIdx].second = Copy;
      LastMIToRegion.erase(IP.InsertNear);
      LastMIToRegion[Copy] = RegionIdx;
    } else if (!IP.IsDefSide && FirstMIToRegion.contains(IP.InsertNear)) {
      unsigned RegionIdx = FirstMIToRegion[IP.InsertNear];
      DAG.Regions[RegionIdx].first = Copy;
      FirstMIToRegion.erase(IP.InsertNear);
      FirstMIToRegion[Copy] = RegionIdx;
    }
  }
}
```

#### 2.3: Add Region Context to ConnectionPoint
```cpp
struct ConnectionPoint {
  // ... existing fields ...
  std::optional<unsigned> RegionIdx;  // If at boundary
  bool IsRegionStart;                  // true if first MI, false if last MI
};
```

### Testing Phase 2
- Verify region boundaries are correctly maintained
- Check that schedulers can still operate on modified regions
- Run full scheduling pipeline tests

**Milestone:** Region boundaries correctly tracked.

---

## Phase 3: PHI Node Support

### Goal
Handle PHI nodes correctly.

### Tasks

#### 3.1: Implement PHI Classification
```cpp
void classifyPHIDef(VNInfo *PhiVNI, LiveRangePartition &Partition) {
  // Analyze uses to determine partition
  bool HasAGPRUse = false;
  bool HasVGPRUse = false;
  
  // Scan all uses reached by this PHI
  for (use of PHI value) {
    if (TII->isMAI(*use)) HasAGPRUse = true;
    else HasVGPRUse = true;
  }
  
  // Conservative: if mixed, treat as VGPR
  // Copies will be inserted at AGPR uses
  if (HasAGPRUse && !HasVGPRUse) {
    Partition.IsAGPR[PhiVNI] = true;
  } else {
    Partition.IsAGPR[PhiVNI] = false;
  }
}
```

#### 3.2: Handle PHI Incoming Values
When an interference is detected at a PHI use, the copy should be inserted in the predecessor block.

Update `findConnectionPoints`:
```cpp
if (UseMI->isPHI()) {
  // For each incoming value that's in wrong partition:
  unsigned OpIdx = UseMO.getOperandNo();
  unsigned PredIdx = (OpIdx - 1) / 2;
  MachineBasicBlock *PredMBB = UseMI->getOperand(OpIdx + 1).getMBB();
  
  // Insert copy before branch in PredMBB
  IP.InsertNear = PredMBB->getFirstTerminator();
  IP.IsDefSide = true;  // After last non-terminator
}
```

#### 3.3: Remove PHI Bail-out
Remove the `if (hasPHI(...)) continue;` guards from Phase 1.

### Testing Phase 3
- PHI with single incoming value needing copy (Example 4)
- Loop-carried dependencies (Example 5)
- PHI with multiple incoming values, some needing copies

**Milestone:** PHI nodes fully supported.

---

## Phase 4: Optimization and Robustness

### Goal
Handle edge cases and optimize copy placement.

### Tasks

#### 4.1: Optimize Cross-Block Copies
When multiple uses in the same block need the same copy:
- Group interference points by block
- Find earliest use per block
- Insert single copy per block

```cpp
void optimizeCopyPlacement(SmallVectorImpl<ConnectionPoint> &Points) {
  // Group by (SrcReg, DstReg, Block)
  // For each group, find optimal insertion point
  // Replace group with single interference point
}
```

#### 4.2: Handle Subregisters (Basic)
Ensure tied operands work correctly:
```cpp
if (MI->getOperand(DefIdx).isTied()) {
  unsigned UseIdx = MI->findTiedOperandIdx(DefIdx);
  // Ensure both are classified the same
}
```

#### 4.3: Improve Debugging
Add comprehensive LLVM_DEBUG output:
```cpp
LLVM_DEBUG({
  dbgs() << "Splitting rewrite for " << printReg(Reg, TRI) << "\n";
  dbgs() << "  AGPR VNIs: ";
  for (auto [VNI, IsAGPR] : Partition.IsAGPR)
    if (IsAGPR) dbgs() << VNI->id << " ";
  dbgs() << "\n  VGPR VNIs: ";
  for (auto [VNI, IsAGPR] : Partition.IsAGPR)
    if (!IsAGPR) dbgs() << VNI->id << " ";
  dbgs() << "\n  Interferences: " << Points.size() << "\n";
});
```

#### 4.4: Add Verification
```cpp
void verifyPartition(const LiveRangePartition &Partition) {
  // Check all VNIs are classified
  // Check no overlapping segments between partitions
  // Check all uses are reachable
  
  if (VerifyFailure) {
    report_fatal_error("Invalid partition");
  }
}
```

### Testing Phase 4
- Complex control flow graphs
- Stress tests with many MFMAs
- Negative tests (should abort or handle gracefully)

**Milestone:** Production-ready implementation.

---

## Phase 5: Advanced Features (Optional)

### 5.1: Subregister Lane Support
Use `LiveInterval::SubRange` for lane-level splitting.

### 5.2: Integration with Cost Model
**Key insight:** With deferred copy insertion, we have a natural checkpoint to calculate cost.

Eliminate the speculative rewrite in `getRewriteCost()` and instead:
```cpp
bool rewriteWithSplitting(...) {
  // Step 1-3: Partition registers, generate copy descriptors
  SmallVector<ConnectionPoint> AllCopyDescriptors;
  for (each MFMA) {
    partition registers...
    generate copy descriptors...
    AllCopyDescriptors.append(...);
  }
  
  // NEW: Step 3.5: Calculate cost from actual copy descriptors
  unsigned CopyCost = calculateCost(AllCopyDescriptors);
  if (CopyCost > Threshold) {
    // Haven't modified IR yet, can safely abort
    return false;
  }
  
  // Step 4-6: Proceed with rewrite (update operands, insert copies)
  updateOperands(...);
  insertCopies(AllCopyDescriptors, ...);
}
```

**Benefits:**
- Single rewrite path for both cost estimation and execution
- Cost based on actual algorithm, not heuristics
- No duplicate work (no speculative rewrite)
- Natural abort point before committing changes

### 5.3: Copy Coalescing
After insertion, try to coalesce redundant copies:
```cpp
void coalesceCopies() {
  for (each inserted copy) {
    if (canCoalesce(Copy)) {
      coalesce(Copy);
    }
  }
}
```

---

## Estimated Timeline

- **Phase 1:** 2-3 days (basic infrastructure)
- **Phase 2:** 1 day (region boundaries)
- **Phase 3:** 2-3 days (PHI support)
- **Phase 4:** 2-3 days (optimization and robustness)
- **Phase 5:** Optional, 3-5 days

**Total:** 1-2 weeks for production-ready implementation.

---

## Success Criteria

1. **Correctness:**
   - Passes all existing RewriteMFMAForm tests
   - No regressions in scheduler tests
   - LiveInterval verification passes

2. **Performance:**
   - Same or better register pressure as current implementation
   - Same or better copy placement (measured by copies in loops)

3. **Code Quality:**
   - <200 lines for core implementation
   - 2-3 primary data structures (vs 7 currently)
   - Clear separation of concerns
   - Comprehensive debug output

4. **Maintainability:**
   - Algorithm clearly documented
   - Each function has single responsibility
   - Easy to extend (e.g., for subregister lanes)

---

## Migration Strategy

### Option A: Side-by-side
Keep both implementations:
```cpp
bool rewrite(...) {
  if (UseSplittingRewrite)
    return rewriteWithSplitting(...);
  else
    return rewriteOriginal(...);
}
```
Controlled by flag, default to new implementation after testing.

### Option B: Direct replacement
Delete old implementation entirely, replace with new.

**Recommendation:** Option A for initial rollout, Option B after bake-in period.

---

## Rollback Plan

If serious issues found:
1. Flip flag to use old implementation
2. Investigate and fix
3. Re-enable new implementation

Keep old code in tree for at least 1-2 releases.
