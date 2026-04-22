//===- GCNPartitionMFMARewrite.h - Partition-based MFMA Rewrite -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Public interface for MFMA rewrite using live range partitioning.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AMDGPU_GCNPARTITIONMFMAREWRITE_H
#define LLVM_LIB_TARGET_AMDGPU_GCNPARTITIONMFMAREWRITE_H

#include "llvm/ADT/ArrayRef.h"
#include <utility>

namespace llvm {

class LiveIntervals;
class MachineFunction;
class MachineInstr;
class MachineRegisterInfo;
class SIInstrInfo;
class TargetInstrInfo;
class TargetRegisterInfo;

/// Rewrite MFMA instructions using live range partitioning.
///
/// This function partitions each rewritten register's live range into
/// AGPR and VGPR segments, then inserts copies at connection points.
///
/// Complete Phase 1-4 implementation:
/// - Phase 1: Core infrastructure (partitioning, interference, copies)
/// - Phase 2: Region boundary tracking.
/// - Phase 3: PHI node support.
/// - Phase 4: Copy optimization, verification, tied operands.
///
/// \param RewriteCands List of MFMA instructions and their original opcodes.
/// \param MF Machine function being processed.
/// \param LIS LiveIntervals analysis.
/// \param TII Target instruction info.
/// \param TRI Target register info.
/// \param SII SI-specific instruction info.
/// \returns true if any rewrites were performed.
bool rewriteWithPartitioning(
    ArrayRef<std::pair<MachineInstr *, unsigned>> RewriteCands,
    MachineFunction &MF, LiveIntervals *LIS, const TargetInstrInfo *TII,
    const TargetRegisterInfo *TRI, const SIInstrInfo *SII);

} // namespace llvm

#endif // LLVM_LIB_TARGET_AMDGPU_GCNPARTITIONMFMAREWRITE_H
