# Integration with GCNSchedStrategy

## Option 1: Direct Replacement (Testing)

For initial testing, directly replace the rewrite() call in `RewriteMFMAFormStage::initGCNSchedStage()`:

```cpp
// In GCNSchedStrategy.cpp, line ~1381
// Current:
return rewrite(RewriteCands);

// Replace with:
#include "splitting_rewrite/SplittingRewrite.h"
return rewriteWithSplitting(RewriteCands, DAG.LIS, DAG.MRI, TII, SRI, TII);
```

## Option 2: Conditional Flag

Add a command-line option to choose between implementations:

```cpp
static cl::opt<bool> UseSplittingRewrite(
    "amdgpu-use-splitting-rewrite",
    cl::desc("Use live range splitting for MFMA rewrite"),
    cl::init(false), cl::Hidden);

// In RewriteMFMAFormStage::initGCNSchedStage():
if (UseSplittingRewrite)
  return rewriteWithSplitting(RewriteCands, DAG.LIS, DAG.MRI, TII, SRI, TII);
else
  return rewrite(RewriteCands);
```

## Option 3: Wrapper Method

Add new method to RewriteMFMAFormStage:

```cpp
class RewriteMFMAFormStage {
  // ... existing members ...
  
  bool rewrite(const std::vector<std::pair<MachineInstr *, unsigned>> &RewriteCands);
  bool rewriteWithSplitting(const std::vector<std::pair<MachineInstr *, unsigned>> &RewriteCands);
};

bool RewriteMFMAFormStage::rewriteWithSplitting(...) {
  return llvm::rewriteWithSplitting(RewriteCands, DAG.LIS, DAG.MRI, TII, SRI, TII);
}
```

## Current Issues to Fix

### 1. MachineFunction Access

The `rewriteWithSplitting` needs `MachineFunction` to call `LIS->reanalyze(MF)`.

Fix: Add MF parameter:
```cpp
bool rewriteWithSplitting(
    ArrayRef<std::pair<MachineInstr *, unsigned>> RewriteCands,
    MachineFunction &MF,  // Add this
    LiveIntervals *LIS,
    ...
```

### 2. Region LiveIns Update

After LIS reanalysis, need to update region live-ins:

```cpp
// At end of rewriteWithSplitting:
LIS->reanalyze(MF);

// Update region live-ins (this is DAG-specific, so needs to be in caller)
// RegionPressureMap LiveInUpdater(&DAG, false);
// LiveInUpdater.buildLiveRegMap();
// for (unsigned Region = 0; Region < DAG.Regions.size(); Region++)
//   DAG.LiveIns[Region] = LiveInUpdater.getLiveRegsForRegionIdx(Region);
```

Better: Return success/failure, let caller update regions:

```cpp
bool RewriteMFMAFormStage::initGCNSchedStage() {
  // ... existing code ...
  
  if (UseSplittingRewrite) {
    if (!llvm::rewriteWithSplitting(RewriteCands, DAG.MF, DAG.LIS, ...))
      return false;
      
    // Update regions
    DAG.LIS->reanalyze(DAG.MF);
    RegionPressureMap LiveInUpdater(&DAG, false);
    LiveInUpdater.buildLiveRegMap();
    for (unsigned Region = 0; Region < DAG.Regions.size(); Region++)
      DAG.LiveIns[Region] = LiveInUpdater.getLiveRegsForRegionIdx(Region);
    
    return true;
  }
  
  return rewrite(RewriteCands);
}
```

## Build Integration

### CMakeLists.txt

Need to add SplittingRewrite.cpp to the build:

```cmake
# In llvm/lib/Target/AMDGPU/CMakeLists.txt
add_llvm_target(AMDGPUCodeGen
  ...
  splitting_rewrite/SplittingRewrite.cpp
  ...
)
```

Or create a separate library:

```cmake
add_llvm_library(AMDGPUSplittingRewrite
  splitting_rewrite/SplittingRewrite.cpp
  
  LINK_COMPONENTS
  CodeGen
  Core
  Support
)

# Then link it
target_link_libraries(AMDGPUCodeGen AMDGPUSplittingRewrite)
```

## Testing Strategy

1. **Compile test:**
   ```bash
   cd build
   ninja AMDGPUCodeGen
   ```

2. **Simple IR test:**
   Create a simple test case with MFMA instructions:
   ```ll
   ; RUN: llc -march=amdgcn -mcpu=gfx90a -amdgpu-use-splitting-rewrite < %s | FileCheck %s
   
   define void @simple_mfma(<32 x float> addrspace(1)* %out, <32 x float> %a, <32 x float> %b) {
     %result = call <32 x float> @llvm.amdgcn.mfma.f32.32x32x1f32(float %a, float %b, <32 x float> zeroinitializer, i32 0, i32 0, i32 0)
     store <32 x float> %result, <32 x float> addrspace(1)* %out
     ret void
   }
   ```

3. **Debug verification:**
   ```bash
   llc -march=amdgcn -mcpu=gfx90a -amdgpu-use-splitting-rewrite \
       -debug-only=gcn-sched-splitting-rewrite \
       test.ll -o test.s 2>&1 | less
   ```

4. **Compare with original:**
   ```bash
   # Original implementation
   llc -march=amdgcn -mcpu=gfx90a test.ll -o test_orig.s
   
   # Splitting rewrite
   llc -march=amdgcn -mcpu=gfx90a -amdgpu-use-splitting-rewrite test.ll -o test_split.s
   
   # Compare
   diff test_orig.s test_split.s
   ```

## Next Steps

1. Fix the MachineFunction parameter issue
2. Update the signature to match what RewriteMFMAFormStage can provide
3. Add the command-line flag
4. Create CMakeLists.txt entry
5. Build and test with simple cases
