# Splitting Rewrite - File Index

Quick navigation for the live range splitting-based MFMA rewrite implementation.

---

## 🚀 Start Here

| File | Purpose |
|------|---------|
| [QUICK_START.md](QUICK_START.md) | **Quick reference** - Build, test, and verify |
| [STATUS.md](STATUS.md) | **Current status** - What's done, what's next |
| [README.md](README.md) | **Project overview** - Problem, solution, advantages |

---

## 📖 Design & Planning

| File | Purpose |
|------|---------|
| [design.md](design.md) | **Detailed algorithm** - Data structures, step-by-step |
| [examples.md](examples.md) | **7 worked examples** - Visual walkthroughs |
| [challenges.md](challenges.md) | **8 challenges** - Problems and solutions |
| [implementation_plan.md](implementation_plan.md) | **5-phase plan** - Incremental rollout |

---

## 💻 Implementation

| File | Purpose |
|------|---------|
| [SplittingRewrite.h](SplittingRewrite.h) | **Public interface** - API definition |
| [SplittingRewrite.cpp](SplittingRewrite.cpp) | **Core implementation** - ~450 lines |

---

## 🧪 Testing

| File | Purpose |
|------|---------|
| [test_phase1.md](test_phase1.md) | **Test strategy** - Unit & integration tests |
| [test_build.sh](test_build.sh) | **Build script** - Automated build & test |
| ../../test/CodeGen/AMDGPU/splitting-rewrite-*.mir | **MIR tests** - 3 test cases |

---

## 🔧 Integration

| File | Purpose |
|------|---------|
| [integration_notes.md](integration_notes.md) | **Integration guide** - How to wire into GCNSchedStrategy |
| [INTEGRATION_COMPLETE.md](INTEGRATION_COMPLETE.md) | **Build integration** - CMake, flags, troubleshooting |
| ../CMakeLists.txt | **Build config** - Modified to include SplittingRewrite.cpp |
| ../GCNSchedStrategy.cpp | **Runtime integration** - Flag and call site |

---

## 📝 Documentation

| File | Lines | Purpose |
|------|-------|---------|
| README.md | ~100 | Project overview |
| design.md | ~400 | Algorithm design |
| examples.md | ~500 | Worked examples |
| challenges.md | ~400 | Implementation challenges |
| implementation_plan.md | ~300 | Phased rollout |
| test_phase1.md | ~250 | Testing strategy |
| integration_notes.md | ~200 | Integration guide |
| phase1_complete.md | ~200 | Phase 1 summary |
| INTEGRATION_COMPLETE.md | ~350 | Build integration |
| QUICK_START.md | ~100 | Quick reference |
| STATUS.md | ~300 | Current status |
| INDEX.md | ~150 | This file |
| **Total** | **~3,250** | |

---

## 📂 Directory Structure

```
splitting_rewrite/
├── INDEX.md                      # This file - Navigation
├── QUICK_START.md                # Quick reference
├── STATUS.md                     # Current status
├── README.md                     # Project overview
├── design.md                     # Algorithm design
├── examples.md                   # Worked examples  
├── challenges.md                 # Implementation challenges
├── implementation_plan.md        # 5-phase plan
├── test_phase1.md                # Testing strategy
├── integration_notes.md          # Integration guide
├── phase1_complete.md            # Phase 1 summary
├── INTEGRATION_COMPLETE.md       # Build integration
├── SplittingRewrite.h            # Public interface
├── SplittingRewrite.cpp          # Implementation
└── test_build.sh                 # Build script
```

---

## 🎯 By Task

### I want to...

**Understand the approach**
→ [README.md](README.md), [design.md](design.md), [examples.md](examples.md)

**Build and test**
→ [QUICK_START.md](QUICK_START.md), [test_build.sh](test_build.sh)

**See what's done**
→ [STATUS.md](STATUS.md), [phase1_complete.md](phase1_complete.md)

**Integrate with existing code**
→ [integration_notes.md](integration_notes.md), [INTEGRATION_COMPLETE.md](INTEGRATION_COMPLETE.md)

**Understand implementation details**
→ [SplittingRewrite.cpp](SplittingRewrite.cpp), [design.md](design.md)

**Write tests**
→ [test_phase1.md](test_phase1.md), MIR test files

**Plan next phases**
→ [implementation_plan.md](implementation_plan.md), [challenges.md](challenges.md)

**Debug issues**
→ [INTEGRATION_COMPLETE.md](INTEGRATION_COMPLETE.md#troubleshooting), [challenges.md](challenges.md)

---

## 📊 Statistics

- **Total files:** 14 (12 docs + 2 code)
- **Documentation:** ~3,250 lines
- **Implementation:** ~450 lines
- **Test cases:** 3 MIR files
- **Phases planned:** 5
- **Current phase:** 1 (Complete ✅)

---

## 🏁 Quick Commands

```bash
# Navigate to directory
cd /work3/tlinthic/llvm/llvm-project/llvm/lib/Target/AMDGPU/splitting_rewrite

# Build
./test_build.sh
# OR
cd /work3/tlinthic/llvm/build && ninja AMDGPUCodeGen

# Test
cd /work3/tlinthic/llvm/build
bin/llc -march=amdgcn -mcpu=gfx90a \
    -run-pass=machine-scheduler \
    -amdgpu-use-splitting-rewrite \
    -debug-only=gcn-sched-splitting-rewrite \
    ../llvm-project/llvm/test/CodeGen/AMDGPU/splitting-rewrite-simple.mir

# Read docs
less README.md
less QUICK_START.md
less design.md
```

---

## 🔗 Related Files (Outside This Directory)

| File | Location | Purpose |
|------|----------|---------|
| CMakeLists.txt | ../CMakeLists.txt | Build configuration |
| GCNSchedStrategy.cpp | ../GCNSchedStrategy.cpp | Runtime integration |
| GCNSchedStrategy.h | ../GCNSchedStrategy.h | Class definitions |
| Test cases | ../../test/CodeGen/AMDGPU/ | MIR test files |

---

## 📚 Reading Order

### For Understanding
1. README.md - Get the big picture
2. design.md - Understand the algorithm
3. examples.md - See it in action
4. challenges.md - Understand the complexity

### For Building
1. QUICK_START.md - Quick reference
2. INTEGRATION_COMPLETE.md - Detailed build guide
3. test_build.sh - Run the script

### For Development
1. STATUS.md - Current state
2. implementation_plan.md - What's next
3. SplittingRewrite.cpp - See the code
4. challenges.md - Known issues

---

**Last Updated:** 2026-04-07  
**Phase:** 1 (Core Infrastructure) - Complete ✅
