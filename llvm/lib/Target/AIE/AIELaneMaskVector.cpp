//===- AIELaneMaskVector.cpp - Lane mask vector implementation -----------===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2026 Advanced Micro Devices, Inc. or its affiliates
//
//===----------------------------------------------------------------------===//
//
// This file implements a vector-like container for lane masks that provides
// safe out-of-range access and common operations.
//
//===----------------------------------------------------------------------===//

#include "AIELaneMaskVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cassert>

using namespace llvm;

namespace llvm {
namespace AIE {

LaneMaskVector::LaneMaskVector(size_t Size) : Masks(Size) {}

LaneMaskVector::LaneMaskVector(size_t Size, LaneBitmask InitialValue)
    : Masks(Size, InitialValue) {}

size_t LaneMaskVector::size() const { return Masks.size(); }

bool LaneMaskVector::empty() const { return Masks.empty(); }

LaneBitmask &LaneMaskVector::operator[](size_t Index) {
  assert(Index < Masks.size() && "Index out of range");
  return Masks[Index];
}

const LaneBitmask &LaneMaskVector::operator[](size_t Index) const {
  assert(Index < Masks.size() && "Index out of range");
  return Masks[Index];
}

LaneBitmask LaneMaskVector::at(size_t Index) const {
  if (Index >= Masks.size()) {
    return LaneBitmask::getNone();
  }
  return Masks[Index];
}

const SmallVector<LaneBitmask, 8> &LaneMaskVector::getMasks() const {
  return Masks;
}

LaneMaskVector &LaneMaskVector::operator|=(const LaneMaskVector &Other) {
  // Determine the maximum size needed
  size_t MaxSize = std::max(Masks.size(), Other.Masks.size());

  // Extend this vector if needed
  if (MaxSize > Masks.size()) {
    Masks.resize(MaxSize);
  }

  // Union using at() which returns empty for out-of-bounds
  for (size_t I = 0; I < MaxSize; ++I) {
    Masks[I] |= Other.at(I);
  }
  return *this;
}

LaneMaskVector &LaneMaskVector::operator&=(const LaneMaskVector &Other) {
  // Use at() which returns empty for out-of-bounds
  for (size_t I = 0; I < Masks.size(); ++I) {
    Masks[I] &= Other.at(I);
  }
  return *this;
}

LaneMaskVector &LaneMaskVector::operator-=(const LaneMaskVector &Other) {
  // Use at() which returns empty for out-of-bounds
  for (size_t I = 0; I < Masks.size(); ++I) {
    Masks[I] &= ~Other.at(I);
  }
  return *this;
}

LaneMaskVector LaneMaskVector::operator|(const LaneMaskVector &Other) const {
  LaneMaskVector Result = *this;
  Result |= Other;
  return Result;
}

LaneMaskVector LaneMaskVector::operator&(const LaneMaskVector &Other) const {
  LaneMaskVector Result = *this;
  Result &= Other;
  return Result;
}

LaneMaskVector LaneMaskVector::operator-(const LaneMaskVector &Other) const {
  LaneMaskVector Result = *this;
  Result -= Other;
  return Result;
}

bool LaneMaskVector::overlaps(const LaneMaskVector &Other) const {
  size_t MinSize = std::min(Masks.size(), Other.Masks.size());
  for (size_t I = 0; I < MinSize; ++I) {
    if ((Masks[I] & Other.Masks[I]).any()) {
      return true;
    }
  }
  return false;
}

bool LaneMaskVector::any() const {
  return llvm::any_of(Masks, [](LaneBitmask M) { return M.any(); });
}

bool LaneMaskVector::none() const {
  return llvm::none_of(Masks, [](LaneBitmask M) { return M.any(); });
}

void LaneMaskVector::dump() const {
  print(dbgs());
  dbgs() << '\n';
}

void LaneMaskVector::print(raw_ostream &OS) const {
  OS << "[";
  for (size_t I = 0; I < Masks.size(); ++I) {
    if (I > 0)
      OS << ", ";
    OS << PrintLaneMask(Masks[I]);
  }
  OS << "]";
}

} // namespace AIE
} // namespace llvm
