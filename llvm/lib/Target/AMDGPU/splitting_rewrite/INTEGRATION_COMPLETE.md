# Phase 1 Build Integration Complete

## Summary

The live range splitting-based MFMA rewrite has been fully integrated into the LLVM build system and is ready for testing.

## Changes Made

### 1. Build System Integration

**File:** `llvm/lib/Target/AMDGPU/CMakeLists.txt`
- Added `splitting_rewrite/SplittingRewrite.cpp` to the AMDGPUCodeGen target

### 2. Code Integration

**File:** `llvm/lib/Target/AMDGPU/GCNSchedStrategy.cpp`

**Added includes:**
```cpp
#include "splitting_rewrite/SplittingRewrite.h"
```

**Added command-line flag:**
```cpp
static cl::opt<bool> UseSplittingRewrite(
    "amdgpu-use-splitting-rewrite", cl::Hidden,
    cl::desc("Use live range splitting for MFMA rewrite (Phase 1 prototype)"),
    cl::init(false));
```

**Modified `RewriteMFMAFormStage::initGCNSchedStage()`:**
- Added conditional branch to use splitting rewrite when flag is enabled
- Updates region live-ins after reanalysis
- Falls back to original implementation when flag is false

### 3. Test Cases Created

**Location:** `llvm/test/CodeGen/AMDGPU/`

1. **splitting-rewrite-simple.mir**
   - Tests: non-MFMA def → MFMA use → non-MFMA use
   - Expected: 2 copies (VGPR→AGPR, AGPR→VGPR)

2. **splitting-rewrite-chain.mir**
   - Tests: MFMA chain with accumulator
   - Expected: 1 copy (VGPR→AGPR), no copy between MFMAs

3. **splitting-rewrite-multiple-uses.mir**
   - Tests: Multiple MFMA uses of same register
   - Expected: Single copy serves all uses (deduplication)

### 4. Build & Test Script

**File:** `splitting_rewrite/test_build.sh`
- Automated build and test execution
- Generates debug output
- Provides next steps

## How to Build

### Option 1: Incremental Build
```bash
cd /work3/tlinthic/llvm/build  # or your build directory
ninja AMDGPUCodeGen
```

### Option 2: Full Rebuild
```bash
cd /work3/tlinthic/llvm/build
ninja clean
ninja AMDGPUCodeGen
```

### Option 3: Using Test Script
```bash
cd /work3/tlinthic/llvm/llvm-project/llvm/lib/Target/AMDGPU/splitting_rewrite
./test_build.sh
```

## How to Test

### Manual Testing

**1. Enable debug output:**
```bash
cd /work3/tlinthic/llvm/build

bin/llc -march=amdgcn -mcpu=gfx90a \
    -run-pass=machine-scheduler \
    -amdgpu-use-splitting-rewrite \
    -debug-only=gcn-sched-splitting-rewrite \
    ../llvm-project/llvm/test/CodeGen/AMDGPU/splitting-rewrite-simple.mir \
    -o /tmp/test-output.mir 2>&1 | less
```

**2. Compare with original implementation:**
```bash
# Original
bin/llc -march=amdgcn -mcpu=gfx90a \
    -run-pass=machine-scheduler \
    splitting-rewrite-simple.mir \
    -o /tmp/original.mir

# Splitting rewrite
bin/llc -march=amdgcn -mcpu=gfx90a \
    -run-pass=machine-scheduler \
    -amdgpu-use-splitting-rewrite \
    splitting-rewrite-simple.mir \
    -o /tmp/splitting.mir

# Compare
diff /tmp/original.mir /tmp/splitting.mir
```

### Running Test Suite

```bash
cd /work3/tlinthic/llvm/build

# Run specific tests
bin/llvm-lit ../llvm-project/llvm/test/CodeGen/AMDGPU/splitting-rewrite-*.mir

# Run all AMDGPU tests (to check for regressions)
bin/llvm-lit ../llvm-project/llvm/test/CodeGen/AMDGPU/
```

## Debug Output to Expect

When running with `-debug-only=gcn-sched-splitting-rewrite`, you should see:

```
=== Starting live range splitting rewrite ===
Processing N candidates

--- Processing MFMA: <instruction>
Partitioning dst <register>
Partition of <register>:
  AGPR reg: <new register>
  VGPR reg: <new register>
  AGPR VNIs: <vni ids>
  VGPR VNIs: <vni ids>

Finding interferences for <register>
  Use at <slot>: needs AGPR, reached by VGPR VNI <id>
  -> VGPR→AGPR copy needed at <slot>
Found N interference points:
  <slot>: <src> -> <dst> (def/use side)

Inserting N copies
  Inserted: COPY <instruction>

Updating operands for <register>
  Use at <slot> -> <new register>
  Def at <slot> -> <new register>

=== Rewrite complete ===
Rewritten: N
Skipped (PHI): M
Reanalyzing LiveIntervals
```

## Verification Checklist

After building, verify:

- [ ] Build completes without errors
- [ ] No new warnings introduced
- [ ] Flag `-amdgpu-use-splitting-rewrite` is recognized
- [ ] Debug output is generated when flag is used
- [ ] Partitions show correct AGPR/VGPR classification
- [ ] Interference points detected correctly
- [ ] Copies inserted at expected locations
- [ ] Operands updated correctly
- [ ] No crashes or assertions
- [ ] MIR output is valid

## Expected Behavior

### Phase 1 Limitations

The implementation will:
- ✅ Handle simple MFMA rewrites without PHI nodes
- ✅ Skip any register with PHI definitions (gracefully)
- ✅ Insert copies at interference points
- ✅ Deduplicate redundant copies
- ⚠️ May break scheduler regions (Phase 2 will fix)
- ⚠️ Print "Skipped (PHI)" for complex control flow

### When to Use This Flag

**Use `-amdgpu-use-splitting-rewrite` for:**
- Testing Phase 1 implementation
- Simple functions without PHI nodes
- Comparing outputs with original implementation
- Development and debugging

**DO NOT use for:**
- Production builds (Phase 1 is prototype)
- Complex kernels with PHI nodes (will skip)
- Performance benchmarking (incomplete)

## Troubleshooting

### Build Errors

**Error: "No such file or directory: splitting_rewrite/SplittingRewrite.h"**
- Check that all files are in correct locations
- Verify CMakeLists.txt was saved correctly
- Try full rebuild: `ninja clean && ninja AMDGPUCodeGen`

**Error: "undefined reference to rewriteWithSplitting"**
- SplittingRewrite.cpp may not be compiled
- Check CMakeLists.txt includes the file
- Verify namespace (should be `llvm::`)

### Runtime Errors

**Assertion: "VNI not in partition"**
- Bug in partition logic
- Check debug output for which VNI failed
- May indicate PHI that wasn't filtered

**Crash in updateOperands()**
- LiveInterval may be stale
- Check that LIS->InsertMachineInstrInMaps was called for all copies
- Verify reanalysis happened

**Assertion in LiveIntervals**
- Operand update may have left dangling references
- Check all uses/defs were updated
- Verify original register is no longer used

### Debug Tips

1. **Enable verbose debug:**
   ```bash
   -debug-only=gcn-sched-splitting-rewrite,machine-scheduler
   ```

2. **Verify MIR at each stage:**
   ```bash
   -print-after=machine-scheduler
   ```

3. **Check LiveIntervals:**
   ```bash
   -verify-machineinstrs
   ```

4. **Isolate the issue:**
   - Start with simplest test case
   - Add complexity incrementally
   - Compare with original at each step

## Next Steps

1. **Immediate:**
   - Build and run tests
   - Verify basic functionality
   - Fix any build/runtime issues

2. **Phase 1 Completion:**
   - Test on real kernels (simple ones)
   - Measure copy count vs original
   - Verify register pressure improvement
   - Document any bugs found

3. **Phase 2 Planning:**
   - Region boundary tracking
   - Handle scheduler region updates
   - More comprehensive testing

## Files Summary

### Implementation
- `splitting_rewrite/SplittingRewrite.cpp` - Core implementation (~450 lines)
- `splitting_rewrite/SplittingRewrite.h` - Public interface

### Integration
- `CMakeLists.txt` - Build system integration
- `GCNSchedStrategy.cpp` - Runtime integration

### Testing
- `test/CodeGen/AMDGPU/splitting-rewrite-simple.mir`
- `test/CodeGen/AMDGPU/splitting-rewrite-chain.mir`
- `test/CodeGen/AMDGPU/splitting-rewrite-multiple-uses.mir`
- `splitting_rewrite/test_build.sh`

### Documentation
- `splitting_rewrite/README.md` - Project overview
- `splitting_rewrite/design.md` - Algorithm design
- `splitting_rewrite/examples.md` - Worked examples
- `splitting_rewrite/challenges.md` - Implementation challenges
- `splitting_rewrite/implementation_plan.md` - Phased plan
- `splitting_rewrite/test_phase1.md` - Testing strategy
- `splitting_rewrite/integration_notes.md` - Integration details
- `splitting_rewrite/phase1_complete.md` - Phase 1 summary
- `splitting_rewrite/INTEGRATION_COMPLETE.md` - This file

---

**Status:** Build integration complete, ready for testing!

**Date:** 2026-04-07

**Phase:** 1 (Core Infrastructure)

**Next:** Build and test
