# Register Pressure Issue - Summary of Investigation

## The Problem

Our new partitioning-based MFMA rewrite creates **2.3x more spilling** than the old implementation:

| Metric | Old (opt_bug.s) | New (opt_bug.partition.s) | Ratio |
|--------|-----------------|---------------------------|-------|
| Lines  | 4812            | 5436                      | 1.13x |
| Spills | 172             | 329                       | 1.91x |
| AGPR copies | 832        | 868                       | 1.04x |

## What We've Tried

### 1. Block-Specific VGPRs for ALL AGPR→VGPR Copies
- **Hypothesis**: Creating one VGPRReg per partition creates long live ranges
- **Implementation**: Created separate VGPR virtual register per (AGPR, Block) pair
- **Result**: Spills=371 (2.15x worse!), AGPR copies=692 (better)
- **Conclusion**: Made spilling even worse despite fewer AGPR copies

### 2. Block-Specific VGPRs ONLY for Use-Side Copies  
- **Hypothesis**: Asymmetric approach like old implementation
- **Implementation**: 
  - Keep partition VGPRReg for def-side copies
  - Create block-specific VGPRs only for use-side (IsDefSide=false) copies
- **Result**: Spills=401 (2.3x worse!), AGPR copies=824 (similar)
- **Conclusion**: Still made spilling worse

### 3. Skip Unneeded Partitions
- **Hypothesis**: We're creating unnecessary VGPRs for registers that don't need partitioning
- **Implementation**: 
  - Check if any uses mismatch defs before creating partition
  - Skip partition if all uses match their reaching def's register class
  - Handle AGPR-only case (all MFMA uses) specially
- **Result**: Spills=401, AGPR copies=824 (no change)
- **Conclusion**: Not the root cause; minor efficiency improvement only

## Current Understanding

### Key Differences from Old Implementation

**Old implementation** (GCNSchedStrategy.cpp):
- ONE mapped VGPR per register via RedefMap (reused for reaching defs)
- NEW VGPR per block for cross-block uses
- Special handling for same-block uses

**Our implementation**:
- ONE VGPRReg + ONE AGPRReg per partitioned register
- All uses/defs within a partition share the same virtual registers
- VNInfo-based partitioning determines which VNI goes to which register

### Why Creating More VGPRs Made It Worse

When we tried block-specific VGPRs:
- Total VGPR virtual register count increased significantly
- Even though each had shorter live range, aggregate pressure was higher
- Register allocator couldn't coalesce them efficiently
- More spilling resulted despite fewer AGPR copies

### Hypotheses Not Yet Tested

1. **We're doubling register count**: Every partitioned register becomes 2 registers (AGPR+VGPR), even if one isn't heavily used
2. **Live range structure is fundamentally different**: Our reanalysis creates different live range shapes
3. **Register class constraints**: AGPR/VGPR class differences prevent effective coalescing
4. **Something about VNInfo-based partitioning**: Creates worse interference patterns

## Next Steps

Using llvm-reduce to create a minimal test case that reproduces the issue will help us:
- Understand the exact pattern causing problems
- Debug with smaller, more comprehensible IR
- Compare implementations side-by-side more easily

## Files

- Current implementation: `SplittingRewrite.cpp` (with skip-unneeded-partitions optimization)
- Backup (one VGPR per reg): `SplittingRewrite.cpp.one_vgpr_per_reg`
- Test case: `/work2/tlinthic/bugs/9457_new_jira/opt_bug.ll`
- Old output: `/work2/tlinthic/bugs/9457_new_jira/opt_bug.s` (172 spills)
- New output: `/work2/tlinthic/bugs/9457_new_jira/opt_bug.partition.s` (329 spills)
