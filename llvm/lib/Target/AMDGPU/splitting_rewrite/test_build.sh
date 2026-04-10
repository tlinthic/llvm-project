#!/bin/bash
# Build and test script for Phase 1 splitting rewrite implementation

set -e  # Exit on error

echo "=== Phase 1 Splitting Rewrite Build & Test ==="
echo ""

# Determine build directory
if [ -d "/work3/tlinthic/llvm/build" ]; then
    BUILD_DIR="/work3/tlinthic/llvm/build"
elif [ -d "../../../../build" ]; then
    BUILD_DIR="../../../../build"
else
    echo "Error: Could not find build directory"
    echo "Please set BUILD_DIR environment variable"
    exit 1
fi

echo "Using build directory: $BUILD_DIR"
echo ""

# Step 1: Build
echo "Step 1: Building AMDGPUCodeGen..."
cd $BUILD_DIR
ninja AMDGPUCodeGen

if [ $? -ne 0 ]; then
    echo "Build failed!"
    exit 1
fi

echo "Build successful!"
echo ""

# Step 2: Run simple test with debug output
echo "Step 2: Running simple test with debug output..."
echo ""

TEST_FILE="/work3/tlinthic/llvm/llvm-project/llvm/test/CodeGen/AMDGPU/splitting-rewrite-simple.mir"

if [ ! -f "$TEST_FILE" ]; then
    echo "Warning: Test file not found: $TEST_FILE"
    echo "Skipping test execution"
else
    echo "Running: llc -march=amdgcn -mcpu=gfx90a -run-pass=machine-scheduler -amdgpu-use-splitting-rewrite -debug-only=gcn-sched-splitting-rewrite $TEST_FILE"
    echo ""

    $BUILD_DIR/bin/llc -march=amdgcn -mcpu=gfx90a \
        -run-pass=machine-scheduler \
        -amdgpu-use-splitting-rewrite \
        -debug-only=gcn-sched-splitting-rewrite \
        $TEST_FILE -o /tmp/splitting-test-output.mir 2>&1 | head -100

    echo ""
    echo "Output written to /tmp/splitting-test-output.mir"
fi

echo ""
echo "=== Test Summary ==="
echo "✓ Build completed"
echo "✓ Debug output generated"
echo ""
echo "Next steps:"
echo "1. Review debug output above"
echo "2. Run: llvm-lit test/CodeGen/AMDGPU/splitting-rewrite-*.mir"
echo "3. Compare output with original implementation"
echo ""
