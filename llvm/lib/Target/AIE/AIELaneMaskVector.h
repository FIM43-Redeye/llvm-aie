//===- AIELaneMaskVector.h - Lane mask vector container --------*- C++ -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2026 Advanced Micro Devices, Inc. or its affiliates
//
//===----------------------------------------------------------------------===//
//
// This file defines a vector-like container for lane masks that provides
// safe out-of-range access and common operations.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AIE_AIELANEMASKVECTOR_H
#define LLVM_LIB_TARGET_AIE_AIELANEMASKVECTOR_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/MC/LaneBitmask.h"

namespace llvm {

class raw_ostream;

namespace AIE {

/// A vector-like container for lane masks that provides safe out-of-range
/// access and common operations.
class LaneMaskVector {
private:
  SmallVector<LaneBitmask, 8> Masks;

public:
  /// Construct with given size, all masks initialized to none
  explicit LaneMaskVector(size_t Size = 0);

  /// Construct with given size and initial value
  LaneMaskVector(size_t Size, LaneBitmask InitialValue);

  /// Get the size of the vector
  size_t size() const;

  /// Check if empty
  bool empty() const;

  /// Access element with bounds checking in debug mode
  LaneBitmask &operator[](size_t Index);
  const LaneBitmask &operator[](size_t Index) const;

  /// Safe access - returns empty mask if out of range
  LaneBitmask at(size_t Index) const;

  /// Get the underlying masks
  const SmallVector<LaneBitmask, 8> &getMasks() const;

  /// Union with another vector
  LaneMaskVector &operator|=(const LaneMaskVector &Other);

  /// Intersection with another vector
  LaneMaskVector &operator&=(const LaneMaskVector &Other);

  /// Difference with another vector (this & ~Other)
  LaneMaskVector &operator-=(const LaneMaskVector &Other);

  /// Create union with another vector
  LaneMaskVector operator|(const LaneMaskVector &Other) const;

  /// Create intersection with another vector
  LaneMaskVector operator&(const LaneMaskVector &Other) const;

  /// Create difference with another vector
  LaneMaskVector operator-(const LaneMaskVector &Other) const;

  /// Check if any lanes overlap with another vector
  bool overlaps(const LaneMaskVector &Other) const;

  /// Check if any lane is set
  bool any() const;

  /// Check if no lanes are set
  bool none() const;

  /// Debug dump
  void dump() const;

  /// Print to stream
  void print(raw_ostream &OS) const;
};

} // namespace AIE
} // namespace llvm

#endif // LLVM_LIB_TARGET_AIE_AIELANEMASKVECTOR_H
