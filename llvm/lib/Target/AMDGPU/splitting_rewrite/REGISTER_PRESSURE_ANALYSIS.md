# Register Pressure Analysis - MFMA Rewrite Spilling Issue

## Problem

Our new partitioning-based MFMA rewrite creates significantly more spilling than the old implementation:

**Old implementation (`opt_bug.s`):**
- Lines: 4812
- Spills (buffer_store/load): 172
- AGPR copies: 832

**New implementation (`opt_bug.partition.s`):**
- Lines: 5436
- Spills: 329 (1.9x worse!)
- AGPR copies: 868 (slightly more)

## What We Tried

### Attempt 1: Block-specific VGPRs for ALL AGPR→VGPR copies
- Created separate VGPR virtual register for each (AGPR, Block) pair
- Result: Spills increased to 371 (2.15x worse!), AGPR copies reduced to 692

### Attempt 2: Block-specific VGPRs ONLY for use-side copies
- Keep partition VGPRReg for def-side copies
- Create block-specific VGPRs only for use-side (IsDefSide=false) AGPR→VGPR copies  
- Result: Spills increased to 401 (2.3x worse!), AGPR copies similar at 824

Both attempts made spilling worse, not better!

## Key Observations

1. **Old implementation pattern** (from GCNSchedStrategy.cpp):
   - ONE mapped VGPR per register via RedefMap (reused for reaching defs)
   - NEW VGPR per block for cross-block uses (line 2790, 2748)
   - Special case for same-block uses (line 2748)

2. **Our implementation**:
   - ONE VGPRReg per partition (all uses share it)
   - This creates LONG live ranges spanning multiple blocks
   - Block-specific VGPRs make it worse, not better!

3. **Copy counts**:
   - New has slightly more AGPR copies (868 vs 832) but within noise
   - When we created block-specific VGPRs, we reduced AGPR copies (692) but massively increased spills

## Hypothesis

The problem is NOT just about long VGPR live ranges. Creating many short VGPRs also increases pressure.

Possible issues with our approach:
1. **We partition ALL registers touched by MFMAs**, even ones that don't need it
2. **We create TWO registers (AGPR + VGPR)** for every original register, doubling the count
3. **Register allocator can't coalesce** our partitioned registers back together
4. **LiveInterval reanalysis** might be creating worse live ranges

## Next Steps

Need to understand:
- Which registers are we partitioning that we shouldn't?
- Are there registers with no AGPR uses that still get partitioned?
- Is the live range structure fundamentally different?
- Why can't the register allocator coalesce away the extra copies?

## Files

- Backup of working implementation: `SplittingRewrite.cpp.one_vgpr_per_reg`
- Current restored implementation: `SplittingRewrite.cpp` (same as backup)
