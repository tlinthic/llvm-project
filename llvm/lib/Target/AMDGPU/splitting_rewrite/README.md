# Live Range Splitting-based MFMA Rewrite

This directory contains design notes and implementation for a new approach to the `RewriteMFMAFormStage::rewrite()` function based on live range splitting concepts from register allocation.

## Problem Statement

The current `rewrite()` implementation (342 lines, 7 complex data structures) is difficult to understand, maintain, and modify. It manually tracks reaching defs/uses and manages copy insertion through multiple interleaved phases.

## Proposed Solution

Use live range splitting to partition each rewritten register's `LiveInterval` into two disjoint live ranges:
- **AGPR live range**: All MFMA uses/defs that must be in AGPRs
- **VGPR live range**: All non-MFMA uses/defs that must be in VGPRs

Copies are inserted at **interference points** where one live range flows into the other.

## Key Advantages

1. **Conceptual Clarity**: Makes explicit what's happening - we're splitting registers across two register classes
2. **Proven Infrastructure**: Leverages LLVM's battle-tested `LiveInterval`, `VNInfo`, and potentially `LiveRangeEdit`
3. **Simpler State**: Only need the partition and two new registers, not 7 separate tracking maps
4. **Natural Copy Placement**: Interference points directly determine where copies go
5. **Better Control Flow**: PHI nodes and complex CFG handled by existing infrastructure

## Copy Placement Strategy

To maintain the loop-hoisting property of the current implementation:
- **VGPR→AGPR copies**: Place near the def (often before loop entry)
- **AGPR→VGPR copies**: Place near the use (often after loop exit)

This keeps copies out of loop bodies where MFMAs typically reside.

## Implementation Status

**Phase 1-4: COMPLETE** ✅
- Core infrastructure, region boundaries, PHI support, optimization
- Production-ready with copy count matching original implementation
- See `PHASE4_COMPLETE.md` and `FINAL_STATUS.md`

**Phase 5: Optional enhancements**
- Subregister lane support
- Copy coalescing
- Cost model integration (eliminate speculative rewrite, use copy descriptors)
- See `implementation_plan.md` for details

## Implementation Phases

See `design.md` for detailed design and `implementation_plan.md` for step-by-step plan.

## Files

- `README.md` - This file
- `design.md` - Detailed algorithm design
- `implementation_plan.md` - Step-by-step implementation strategy
- `examples.md` - Worked examples showing the transformation
- `challenges.md` - Known challenges and solutions
