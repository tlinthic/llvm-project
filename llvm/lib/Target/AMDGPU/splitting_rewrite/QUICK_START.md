# Quick Start Guide - Phase 1 Splitting Rewrite

## Build (Choose One)

```bash
# Option 1: Use the test script
cd /work3/tlinthic/llvm/llvm-project/llvm/lib/Target/AMDGPU/splitting_rewrite
./test_build.sh

# Option 2: Manual build
cd /work3/tlinthic/llvm/build
ninja AMDGPUCodeGen
```

## Test

```bash
cd /work3/tlinthic/llvm/build

# Simple test with debug output
bin/llc -march=amdgcn -mcpu=gfx90a \
    -run-pass=machine-scheduler \
    -amdgpu-use-splitting-rewrite \
    -debug-only=gcn-sched-splitting-rewrite \
    ../llvm-project/llvm/test/CodeGen/AMDGPU/splitting-rewrite-simple.mir \
    -o - 2>&1 | less
```

## Compare with Original

```bash
cd /work3/tlinthic/llvm/build

# Original implementation
bin/llc -march=amdgcn -mcpu=gfx90a \
    -run-pass=machine-scheduler \
    ../llvm-project/llvm/test/CodeGen/AMDGPU/splitting-rewrite-simple.mir \
    -o /tmp/original.mir

# Splitting rewrite
bin/llc -march=amdgcn -mcpu=gfx90a \
    -run-pass=machine-scheduler \
    -amdgpu-use-splitting-rewrite \
    ../llvm-project/llvm/test/CodeGen/AMDGPU/splitting-rewrite-simple.mir \
    -o /tmp/splitting.mir

# Compare
diff /tmp/original.mir /tmp/splitting.mir
```

## What to Look For

### In Debug Output
- ✅ Partition shows AGPR/VGPR VNIs correctly classified
- ✅ Interference points detected at correct locations
- ✅ Copies inserted (check count matches expectations)
- ✅ All operands updated

### In MIR Output
- ✅ MFMA instructions changed to AGPR form
- ✅ COPY instructions inserted
- ✅ Register names changed (e.g., %0 → %0_agpr, %0_vgpr)

## Test Cases

1. **splitting-rewrite-simple.mir** - Basic def→use→use pattern
2. **splitting-rewrite-chain.mir** - MFMA chain (accumulator)
3. **splitting-rewrite-multiple-uses.mir** - Copy deduplication

## Common Issues

| Issue | Solution |
|-------|----------|
| Build error: file not found | Check files are in `splitting_rewrite/` subdirectory |
| Undefined reference | Rebuild: `ninja clean && ninja AMDGPUCodeGen` |
| Assertion: VNI not in partition | Bug - check debug output for which VNI |
| Crash in updateOperands | Check all copies have InsertMachineInstrInMaps |
| "Skipped (PHI)" in debug | Expected - Phase 1 bails on PHI nodes |

## Files Modified

- ✅ `CMakeLists.txt` - Added SplittingRewrite.cpp
- ✅ `GCNSchedStrategy.cpp` - Added flag and integration
- ✅ Created `splitting_rewrite/` directory with implementation

## Key Flag

**`-amdgpu-use-splitting-rewrite`**
- Hidden flag (not in `--help`)
- Disabled by default
- Use for Phase 1 testing only

## Quick Checklist

After building:
- [ ] No build errors or warnings
- [ ] Flag is recognized (`llc --help-hidden | grep splitting`)
- [ ] Simple test runs without crashes
- [ ] Debug output shows partitioning
- [ ] Copies are inserted
- [ ] MIR output is valid

## Next Steps

1. Build successfully ✓
2. Run simple test ✓
3. Verify debug output ✓
4. Compare with original ✓
5. Test all 3 test cases ✓
6. Report any issues found
7. Move to Phase 2 (region boundaries)
