# Worked Examples

## Example 1: Simple MFMA with Non-MFMA Def and Use

### Original Code
```
BB1:
  %0:vgpr_32 = LOAD ...               ; VGPR def
  
BB2 (loop):
  %1:vgpr_32 = MFMA %0, %2, %3        ; MFMA use of %0, def of %1
  
BB3:
  STORE %1, ...                       ; Non-MFMA use of %1
```

### Live Intervals (Before)
```
%0: [LOAD.def, MFMA.use]
%1: [MFMA.def, STORE.use]
```

### Step 1: Identify Rewrite Candidates
- MFMA in BB2 can be rewritten to AGPR form

### Step 2: Partition Live Ranges

**For %0 (src2 operand):**
- VGPR partition: [LOAD.def, ...)
- AGPR partition: [..., MFMA.use]
- Interference: VGPR→AGPR at MFMA.use

**For %1 (dst operand):**
- AGPR partition: [MFMA.def, ...)
- VGPR partition: [..., STORE.use]
- Interference: AGPR→VGPR at STORE.use

### Step 3: Insert Copies

**For %0:** VGPR→AGPR copy placed "near def" (after LOAD):
```
BB1:
  %0_vgpr:vgpr_32 = LOAD ...
  %0_agpr:agpr_32 = COPY %0_vgpr     ; VGPR→AGPR copy
```

**For %1:** AGPR→VGPR copy placed "near use" (before STORE):
```
BB3:
  %1_vgpr:vgpr_32 = COPY %1_agpr     ; AGPR→VGPR copy
  STORE %1_vgpr, ...
```

### Step 4: Update Operands

```
BB2 (loop):
  %1_agpr:agpr_32 = MFMA_AGPR %0_agpr, %2, %3
```

### Final Code
```
BB1:
  %0_vgpr:vgpr_32 = LOAD ...
  %0_agpr:agpr_32 = COPY %0_vgpr     ; Outside loop
  
BB2 (loop):
  %1_agpr:agpr_32 = MFMA_AGPR %0_agpr, %2, %3
  
BB3:
  %1_vgpr:vgpr_32 = COPY %1_agpr     ; Outside loop
  STORE %1_vgpr, ...
```

**Result:** Copies placed outside the loop, achieving the same optimization as current implementation.

---

## Example 2: MFMA Chain (Tied Operands)

### Original Code
```
BB1 (loop):
  %0:vgpr_32 = MFMA %1, %2, %0       ; %0 is both src3 and dst (tied)
  %0:vgpr_32 = MFMA %3, %4, %0       ; Chain continues
```

### Analysis
- Both MFMAs are rewrite candidates
- %0 needs to stay in AGPR throughout the chain
- No interference within the loop

### Partition
- AGPR partition: [First MFMA.def, ..., Second MFMA.use]
- VGPR partition: (empty if no non-MFMA uses)

### Final Code
```
BB1 (loop):
  %0_agpr:agpr_32 = MFMA_AGPR %1, %2, %0_agpr
  %0_agpr:agpr_32 = MFMA_AGPR %3, %4, %0_agpr
```

**Result:** No copies needed within the chain.

---

## Example 3: Multiple Uses in Different Blocks

### Original Code
```
BB1:
  %0:vgpr_32 = NON_MFMA ...
  
BB2:
  use %0 in MFMA                     ; Needs AGPR
  
BB3:
  use %0 in NON_MFMA                 ; Needs VGPR
```

### Live Intervals
```
%0: [NON_MFMA.def, MFMA.use in BB2, NON_MFMA.use in BB3]
```

### Partition
- VGPR partition: [NON_MFMA.def, ..., NON_MFMA.use in BB3]
- AGPR partition: [..., MFMA.use in BB2]

### Interference Points
- VGPR→AGPR at MFMA.use in BB2

### Copy Placement
**Strategy 1 (near def):** Insert copy after def in BB1
```
BB1:
  %0_vgpr:vgpr_32 = NON_MFMA ...
  %0_agpr:agpr_32 = COPY %0_vgpr     ; Copy here
  
BB2:
  use %0_agpr in MFMA
  
BB3:
  use %0_vgpr in NON_MFMA
```

**Strategy 2 (near use):** Insert copy at beginning of BB2
```
BB1:
  %0_vgpr:vgpr_32 = NON_MFMA ...
  
BB2:
  %0_agpr:agpr_32 = COPY %0_vgpr     ; Copy here
  use %0_agpr in MFMA
  
BB3:
  use %0_vgpr in NON_MFMA
```

**Current implementation uses Strategy 1** (near def) for VGPR→AGPR to hoist copies out of loops. This example shows that works naturally.

---

## Example 4: PHI Node with Mixed Partitions

### Original Code
```
BB1:
  %0:vgpr_32 = MFMA ...              ; AGPR def
  br BB3
  
BB2:
  %0:vgpr_32 = NON_MFMA ...          ; VGPR def
  br BB3
  
BB3:
  %1:vgpr_32 = PHI %0(BB1), %0(BB2)
  use %1 in MFMA                     ; Needs AGPR
```

### Analysis
- PHI merges AGPR value (from BB1) and VGPR value (from BB2)
- Use in BB3 requires AGPR

### Solution: Insert Copy in BB2
Classify PHI as AGPR (based on use). Insert copy for the VGPR incoming value:

```
BB1:
  %0_agpr:agpr_32 = MFMA ...
  br BB3
  
BB2:
  %0_vgpr:vgpr_32 = NON_MFMA ...
  %0_agpr:agpr_32 = COPY %0_vgpr     ; Copy before branch
  br BB3
  
BB3:
  %1_agpr:agpr_32 = PHI %0_agpr(BB1), %0_agpr(BB2)
  use %1_agpr in MFMA
```

### How This Works in the Algorithm

1. **Partition %0:**
   - In BB1: AGPR partition (MFMA def)
   - In BB2: VGPR partition (NON_MFMA def)

2. **Classify PHI in BB3:**
   - Based on use (MFMA), PHI value is AGPR

3. **Find Interference:**
   - The VGPR def in BB2 reaches an AGPR PHI
   - Interference point: end of BB2

4. **Insert Copy:**
   - VGPR→AGPR copy inserted before branch in BB2

---

## Example 5: Loop-Carried Dependency

### Original Code
```
BB1 (preheader):
  %0:vgpr_32 = LOAD ...              ; Initial value
  
BB2 (loop):
  %1:vgpr_32 = PHI %0(BB1), %2(BB2)
  %2:vgpr_32 = MFMA %1, ...          ; AGPR use and def
  br BB2, BB3
  
BB3 (exit):
  STORE %2, ...                      ; Non-MFMA use
```

### Live Intervals
```
%0: [LOAD.def, PHI.use]
%1: [PHI.def, MFMA.use]
%2: [MFMA.def, PHI.use in BB2, STORE.use in BB3]
```

### Partition

**%0:**
- VGPR partition: [LOAD.def, ...]
- AGPR partition: [..., PHI.use] (PHI classified as AGPR based on MFMA use)
- Interference: VGPR→AGPR at PHI in BB2

**%1:**
- AGPR partition: [PHI.def, MFMA.use]
- (all uses are AGPR)

**%2:**
- AGPR partition: [MFMA.def, ..., PHI.use in BB2]
- VGPR partition: [..., STORE.use in BB3]
- Interference: AGPR→VGPR at STORE.use in BB3

### Final Code
```
BB1 (preheader):
  %0_vgpr:vgpr_32 = LOAD ...
  %0_agpr:agpr_32 = COPY %0_vgpr     ; Copy outside loop
  
BB2 (loop):
  %1_agpr:agpr_32 = PHI %0_agpr(BB1), %2_agpr(BB2)
  %2_agpr:agpr_32 = MFMA_AGPR %1_agpr, ...
  br BB2, BB3
  
BB3 (exit):
  %2_vgpr:vgpr_32 = COPY %2_agpr     ; Copy outside loop
  STORE %2_vgpr, ...
```

**Result:** Both copies placed outside the loop, minimal overhead.

---

## Example 6: Multiple Reaching Defs

### Original Code
```
BB1:
  %0:vgpr_32 = NON_MFMA ...
  br BB3
  
BB2:
  %0:vgpr_32 = NON_MFMA ...
  br BB3
  
BB3:
  use %0 in MFMA
  use %0 in MFMA
```

### Partition
- VGPR partition: [NON_MFMA.def in BB1], [NON_MFMA.def in BB2]
- AGPR partition: [..., MFMA.use, MFMA.use in BB3]

### Interference Points
- BB1 def → BB3 MFMA uses: one interference
- BB2 def → BB3 MFMA uses: one interference

### Copy Placement (near def strategy)
```
BB1:
  %0_vgpr:vgpr_32 = NON_MFMA ...
  %0_agpr:agpr_32 = COPY %0_vgpr
  br BB3
  
BB2:
  %0_vgpr:vgpr_32 = NON_MFMA ...
  %0_agpr:agpr_32 = COPY %0_vgpr
  br BB3
  
BB3:
  use %0_agpr in MFMA
  use %0_agpr in MFMA               ; Single copy serves both uses
```

**Note:** Deduplication ensures we don't insert redundant copies for the multiple uses in BB3.

---

## Example 7: Partial Subregister Write (Unsafe Pattern)

### Original Code
```
BB1:
  %0:vgpr_64 = MFMA ...              ; Full 64-bit AGPR def
  %0.sub0:vgpr_32 = NON_MFMA ...     ; Partial write to low 32 bits
  use %0 in MFMA                     ; Uses full 64 bits
```

### Analysis
This is the pattern detected by `hasSafePartialRedefs()`. The partial subreg write merges:
- High 32 bits from MFMA (would be AGPR)
- Low 32 bits from NON_MFMA (would be VGPR)

### Current Behavior
Rejected by `hasSafePartialRedefs()` at line 2302-2303.

### Proposed Behavior
**Same:** Keep the safety check. This pattern is complex to split correctly and likely rare.

**Future:** Could handle with lane-level splitting using SubRanges, but defer this complexity.

---

## Summary

The live range splitting approach handles all the key cases:
- Simple def-use chains ✓
- MFMA chains with tied operands ✓
- Cross-block uses with mixed register classes ✓
- PHI nodes ✓
- Loop-carried dependencies ✓
- Multiple reaching defs ✓
- Copy placement optimization ✓

And maintains the same safety checks for edge cases like partial subreg writes.
