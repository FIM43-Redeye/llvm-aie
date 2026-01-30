//===- AIERegDefUseTracker.h - Track Register Live Ranges ----------------===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2025-2026 Advanced Micro Devices, Inc. or its affiliates
//
//===----------------------------------------------------------------------===//
//
// This file contains declarations for tracking and analyzing register live
// ranges in a MachineBasicBlock. The tracker performs the following:
// - Identifies register definitions and uses that form live ranges
// - Merges aliasing register accesses into unified live ranges
// - Filters out unsafe ranges (tied operands, live-in/out, implicit uses)
// - Computes appropriate register classes for each live range
// - Optionally replaces physical registers with virtual registers for testing
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AIE_AIEREGDEFUSETRACKER_H
#define LLVM_LIB_TARGET_AIE_AIEREGDEFUSETRACKER_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/CodeGen/Register.h"
#include "llvm/MC/MCRegister.h"

namespace llvm {

struct AIEBaseInstrInfo;
class MachineBasicBlock;
class MachineFunction;
class MachineInstr;
class MachineOperand;
class MachineRegisterInfo;
class TargetRegisterInfo;
class TargetRegisterClass;

/// Represents a register operand with its sub-register index
class RegOperandInfo {
  MachineOperand *Operand;
  unsigned SubRegIdx;

public:
  RegOperandInfo(MachineOperand *Op, unsigned SubIdx = 0)
      : Operand(Op), SubRegIdx(SubIdx) {}

  MachineOperand *getOperand() const { return Operand; }
  unsigned getSubRegIdx() const { return SubRegIdx; }
};

/// Structure representing a live range for a register
/// A live range can have multiple definitions (e.g., when different
/// sub-registers are defined separately) and multiple uses
class RegLiveRange {
  // All definitions that contribute to this live range
  SmallVector<RegOperandInfo, 4> Defs;

  // All uses of this live range
  SmallVector<RegOperandInfo, 4> Uses;

  // Base register for this live range (largest register that covers all
  // operands)
  MCRegister BaseReg = MCRegister::NoRegister;

  // Register class that satisfies all constraints for this live range
  const TargetRegisterClass *RegisterClass = nullptr;

  // Virtual register assigned to this live range (if virtualized)
  Register VReg;

  // Whether this live range is scarce (has exactly 1 available register)
  bool IsScarce = false;

public:
  RegLiveRange() = default;

  void addDef(MachineOperand *DefOp, unsigned SubRegIdx);
  void addUse(MachineOperand *UseOp, unsigned SubRegIdx);

  /// Get the number of definitions
  size_t getNumDefs() const { return Defs.size(); }

  /// Get the number of uses
  size_t getNumUses() const { return Uses.size(); }

  /// Iterator access to definitions
  auto defs() const { return llvm::make_range(Defs.begin(), Defs.end()); }

  /// Iterator access to uses
  auto uses() const { return llvm::make_range(Uses.begin(), Uses.end()); }

  /// Iterator across all defs and uses.
  auto operands() const {
    return llvm::concat<const RegOperandInfo>(Uses, Defs);
  }

  /// Get the base register for this live range
  MCRegister getBaseReg() const { return BaseReg; }

  /// Set the base register for this live range
  void setBaseReg(MCRegister Reg) { BaseReg = Reg; }

  /// Get the register class for this live range
  const TargetRegisterClass *getRegisterClass() const { return RegisterClass; }

  /// Set the register class for this live range
  void setRegisterClass(const TargetRegisterClass *RC) { RegisterClass = RC; }

  /// Get the virtual register assigned to this live range
  Register getVReg() const { return VReg; }

  /// Set the virtual register for this live range
  void setVReg(Register R) { VReg = R; }

  /// Check if this live range is scarce (has exactly 1 available register)
  bool isScarce() const { return IsScarce; }

  /// Set whether this live range is scarce
  void setIsScarce(bool Scarce) { IsScarce = Scarce; }

  // Friend class to allow RegLiveRangeTracker to access internals for merging
  friend class RegLiveRangeTracker;
};

/// Tracker for register live ranges in a MachineBasicBlock
class RegLiveRangeTracker {
  MachineFunction *MF;
  const TargetRegisterInfo *TRI;
  const AIEBaseInstrInfo *TII;

  // List of all live ranges found in the block
  SmallVector<RegLiveRange, 16> LiveRanges;

  // All physical register operands in the block
  SmallVector<MachineOperand *, 32> AllPhysRegOperands;

  // Instruction order mapping for determining earliest operand
  DenseMap<const MachineInstr *, unsigned> InstrOrder;

  // Track whether registers have been virtualized
  mutable bool RegistersVirtualized = false;

  // Cached available physical registers (computed during analyze)
  DenseSet<MCRegister> AvailablePhysRegs;

  // Cached most promising scarce range set (computed during analyze)
  std::vector<const RegLiveRange *> MostPromisingScarceRanges;

  /// Get the sub-register index if AccessReg is a sub-register of BaseReg
  /// Returns 0 if AccessReg is not a sub-register of BaseReg
  unsigned getSubRegIndex(MCRegister AccessReg, MCRegister BaseReg) const;

  /// Compute the alias closure for a register (includes sub and super regs)
  void computeAliasClosure(MCRegister Reg, DenseSet<MCRegister> &Out) const;

  /// Check if a register overlaps with any register in a set
  bool overlapsAnyInSet(MCRegister Reg,
                        const DenseSet<MCRegister> &RegSet) const;

  /// Compute the register class for a live range based on all its operands
  void computeRegisterClass(RegLiveRange &LR) const;

  /// First-stage safety filtering
  bool isCarriedByLiveInOut(const RegLiveRange &LR,
                            const DenseSet<MCRegister> &LiveInAliases,
                            const DenseSet<MCRegister> &LiveOutAliases) const;
  bool startsWithDefInBlock(const RegLiveRange &LR) const;
  bool hasTiedOperands(const RegLiveRange &LR) const;

  /// Second-stage full coverage pruning
  void pruneByFullCoverage();

  /// Merge aliasing live ranges when a definition is encountered
  void mergeAliasingLiveRanges(
      unsigned DefLRIdx, MCRegister DefReg,
      DenseMap<MCRegister, unsigned> &LiveRegs,
      DenseMap<MachineOperand *, unsigned> &OperandToLiveRange);

  /// Helper to find the most promising scarce range set.
  /// Called by analyze() to populate MostPromisingScarceRanges.
  std::vector<const RegLiveRange *> findMostPromisingScarceRanges(
      const DenseSet<MCRegister> &AvailablePhysRegs) const;

public:
  RegLiveRangeTracker(MachineBasicBlock &MBB);

  /// Process a MachineBasicBlock to find all register live ranges
  /// @param MBB The machine basic block to analyze
  /// @param SemanticOrder The semantic instruction order (required - must be
  ///                      non-empty)
  void analyze(MachineBasicBlock &MBB, ArrayRef<MachineInstr *> SemanticOrder);

  /// Get all live ranges
  ArrayRef<RegLiveRange> getLiveRanges() const { return LiveRanges; }

  /// Dump the live range information for debugging
  void dump() const;

  /// Replace filtered physical registers with virtual registers
  /// This modifies the MachineBasicBlock and updates LiveRanges with VReg info
  void virtualizeFilteredPhysRegs();

  /// Get the set of physical registers that would be available for reallocation
  /// Returns the cached value computed during analyze()
  const DenseSet<MCRegister> &getAvailablePhysRegs() const {
    return AvailablePhysRegs;
  }

  /// Rewrite virtual registers to physical registers using the provided
  /// mapping.
  /// @param VRegToPhysMap Mapping from virtual registers to physical registers
  void rewriteToPhysRegs(const DenseMap<Register, MCRegister> &VRegToPhysMap);

  /// Restore original physical registers from virtual registers
  /// Uses the LiveRanges to map VRegs back to their original PhysRegs
  /// This is a convenience method that builds the mapping and calls
  /// rewriteToPhysRegs
  void restoreOriginalPhysRegs();

  /// Check if registers are currently virtualized
  bool areRegistersVirtualized() const;

  /// Filter live ranges based on available physical registers.
  /// Removes live ranges that have only one available physical register
  /// for their register class, as these should stay physical to avoid
  /// pipeliner invalidation.
  /// Uses the cached AvailablePhysRegs computed during analyze().
  void filterByRegisterAvailability();

  /// Clear all state and bring the tracker back to its default constructed
  /// state
  void clear();

  /// Get the most promising scarce range set for packing.
  /// Returns the cached value computed during analyze().
  /// An empty vector signals that no such set could be found.
  const std::vector<const RegLiveRange *> &
  getMostPromisingScarceRanges() const {
    return MostPromisingScarceRanges;
  }
};

} // end namespace llvm

#endif // LLVM_LIB_TARGET_AIE_AIEREGDEFUSETRACKER_H
