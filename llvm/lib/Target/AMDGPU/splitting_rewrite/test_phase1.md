# Phase 1 Testing Plan

## Test Case 1: Simple MFMA with Non-MFMA Def and Use

### MIR Input
```mir
name: simple_mfma
body: |
  bb.0:
    %0:vgpr_32 = V_MOV_B32_e32 0
    
  bb.1:
    %1:vgpr_32 = V_MFMA_F32_32X32X1F32 %0, %0, 0
    
  bb.2:
    %2:vgpr_32 = V_ADD_F32_e32 %1, %1
```

### Expected Transformation
```mir
name: simple_mfma
body: |
  bb.0:
    %0_vgpr:vgpr_32 = V_MOV_B32_e32 0
    %0_agpr:agpr_32 = COPY %0_vgpr        ; VGPR→AGPR copy (src2)
    
  bb.1:
    %1_agpr:agpr_32 = V_MFMA_F32_32X32X1F32_AGPR %0_agpr, %0_agpr, 0
    
  bb.2:
    %1_vgpr:vgpr_32 = COPY %1_agpr        ; AGPR→VGPR copy (dst)
    %2:vgpr_32 = V_ADD_F32_e32 %1_vgpr, %1_vgpr
```

### Verification Points
- [ ] Partition creates 2 new registers per original register
- [ ] VNI classification: V_MOV is VGPR, V_MFMA is AGPR
- [ ] 2 interference points detected (1 for src2, 1 for dst)
- [ ] Copies inserted outside bb.1
- [ ] All operands updated correctly
- [ ] No PHI nodes involved

---

## Test Case 2: MFMA Chain

### MIR Input
```mir
name: mfma_chain
body: |
  bb.0:
    %0:vgpr_32 = V_MOV_B32_e32 0
    
  bb.1:
    %1:vgpr_32 = V_MFMA_F32_32X32X1F32 %0, %0, 0
    %2:vgpr_32 = V_MFMA_F32_32X32X1F32 %0, %0, %1   ; Uses %1 as accumulator
```

### Expected Transformation
```mir
name: mfma_chain
body: |
  bb.0:
    %0_vgpr:vgpr_32 = V_MOV_B32_e32 0
    %0_agpr:agpr_32 = COPY %0_vgpr
    
  bb.1:
    %1_agpr:agpr_32 = V_MFMA_F32_32X32X1F32_AGPR %0_agpr, %0_agpr, 0
    %2_agpr:agpr_32 = V_MFMA_F32_32X32X1F32_AGPR %0_agpr, %0_agpr, %1_agpr
```

### Verification Points
- [ ] No copy between %1 and %2 (both AGPR)
- [ ] Only 1 VGPR→AGPR copy for %0
- [ ] Tied operands handled correctly

---

## Test Case 3: Multiple Uses in Same Block

### MIR Input
```mir
name: multiple_uses
body: |
  bb.0:
    %0:vgpr_32 = V_MOV_B32_e32 0
    %1:vgpr_32 = V_MFMA_F32_32X32X1F32 %0, %0, 0
    %2:vgpr_32 = V_MFMA_F32_32X32X1F32 %0, %0, 0   ; Another use of %0
```

### Expected Transformation
```mir
name: multiple_uses
body: |
  bb.0:
    %0_vgpr:vgpr_32 = V_MOV_B32_e32 0
    %0_agpr:agpr_32 = COPY %0_vgpr
    %1_agpr:agpr_32 = V_MFMA_F32_32X32X1F32_AGPR %0_agpr, %0_agpr, 0
    %2_agpr:agpr_32 = V_MFMA_F32_32X32X1F32_AGPR %0_agpr, %0_agpr, 0
```

### Verification Points
- [ ] Only 1 copy for %0 despite 2 uses
- [ ] Copy deduplication working correctly

---

## Test Case 4: Should Skip (Contains PHI)

### MIR Input
```mir
name: with_phi
body: |
  bb.0:
    %0:vgpr_32 = V_MOV_B32_e32 0
    br %bb.2
    
  bb.1:
    %0:vgpr_32 = V_MOV_B32_e32 1
    br %bb.2
    
  bb.2:
    %1:vgpr_32 = PHI %0, %bb.0, %0, %bb.1
    %2:vgpr_32 = V_MFMA_F32_32X32X1F32 %1, %1, 0
```

### Expected Behavior
- [ ] hasPHIDef() detects PHI for %1
- [ ] partitionLiveRange() returns std::nullopt
- [ ] Candidate skipped gracefully
- [ ] No crash or assertion failure

---

## Unit Tests

### Test hasPHIDef()
```cpp
// Register with PHI
LiveInterval LI_PHI = ...;
VNInfo *PhiVNI = LI_PHI.createDeadDef(..., VNInfo::PHIDef);
assert(hasPHIDef(Reg_PHI, LIS) == true);

// Register without PHI
LiveInterval LI_NoPHI = ...;
VNInfo *NormalVNI = LI_NoPHI.createDeadDef(..., 0);
assert(hasPHIDef(Reg_NoPHI, LIS) == false);
```

### Test classifyVNInfo()
```cpp
// MFMA def with DefIsAGPR=true -> AGPR
MachineInstr *MFMA = /* V_MFMA instruction */;
VNInfo *MfmaVNI = LI.getVNInfoAt(LIS->getInstructionIndex(*MFMA));
assert(classifyVNInfo(MfmaVNI, LIS, TII, /*DefIsAGPR=*/true) == true);

// Non-MFMA def with DefIsAGPR=true -> VGPR
MachineInstr *Add = /* V_ADD instruction */;
VNInfo *AddVNI = LI.getVNInfoAt(LIS->getInstructionIndex(*Add));
assert(classifyVNInfo(AddVNI, LIS, TII, /*DefIsAGPR=*/true) == false);

// Non-MFMA def with DefIsAGPR=false -> VGPR
assert(classifyVNInfo(AddVNI, LIS, TII, /*DefIsAGPR=*/false) == false);
```

### Test deduplicateConnectionPoints()
```cpp
SmallVector<ConnectionPoint> Points;
Points.push_back({Idx1, R1, R2, true, MI1});
Points.push_back({Idx1, R1, R2, true, MI1});  // Duplicate
Points.push_back({Idx2, R1, R2, true, MI2});

deduplicateConnectionPoints(Points);
assert(Points.size() == 2);
```

---

## Integration Test Strategy

1. **Build the prototype:**
   - Compile SplittingRewrite.cpp as a standalone library
   - Link with required LLVM libraries

2. **Create wrapper test:**
   - Small MachineFunction builder
   - Call rewriteWithSplitting()
   - Verify output

3. **Incremental integration:**
   - Add flag to RewriteMFMAFormStage: `-use-splitting-rewrite`
   - If flag set, call rewriteWithSplitting()
   - Else, use original rewrite()

4. **Compare outputs:**
   - Run both implementations on same input
   - Compare register pressure
   - Compare copy count and placement

---

## Debug Checklist

When debugging Phase 1:

- [ ] Enable LLVM_DEBUG for "gcn-sched-splitting-rewrite"
- [ ] Check partition dump shows correct AGPR/VGPR classification
- [ ] Verify interference points match expected locations
- [ ] Confirm copies are inserted at correct positions
- [ ] Verify all operands are updated (no dangling OrigReg references)
- [ ] Check LiveIntervals are valid after reanalysis
- [ ] Ensure no crashes on PHI bail-out path

---

## Known Limitations (Phase 1)

1. **No PHI support** - Will skip any candidate with PHI nodes
2. **No region boundary tracking** - May break scheduler regions
3. **No tied operand special handling** - May not work correctly for accumulators
4. **No subregister support** - Assumes whole-register operations
5. **Minimal error handling** - May assert on unexpected cases

These will be addressed in subsequent phases.
