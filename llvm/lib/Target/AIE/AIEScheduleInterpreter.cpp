//===- AIEScheduleInterpreter.cpp - Schedule-aware itinerary interpreter -===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2025-2026 Advanced Micro Devices, Inc. or its affiliates
//
//===----------------------------------------------------------------------===//
//
// This file implements a schedule-aware interpreter that computes register
// file (RF) occupancy windows from scheduled MachineInstrs and itinerary
// data.
//
//===----------------------------------------------------------------------===//

#include "AIEScheduleInterpreter.h"
#include "AIEBaseInstrInfo.h"
#include "AIELaneMaskVector.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/MC/LaneBitmask.h"
#include "llvm/MC/MCInstrDesc.h"
#include "llvm/MC/MCInstrItineraries.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cassert>
#include <map>
#include <set>
#include <vector>

#define DEBUG_TYPE "aie-schedule-interpreter"

using namespace llvm;

AIEScheduleInterpreter::AIEScheduleInterpreter(const MachineFunction &MF)
    : TII(*MF.getSubtarget().getInstrInfo()),
      TRI(*MF.getSubtarget().getRegisterInfo()), MRI(MF.getRegInfo()),
      Itin(MF.getSubtarget().getInstrItineraryData()) {
  assert(Itin && !Itin->isEmpty() &&
         "Instruction itinerary data must be provided");
}

int AIEScheduleInterpreter::getOperandCycle(const MachineInstr &MI,
                                            unsigned OpIdx) const {

  assert(OpIdx < MI.getNumOperands() && "OpIdx out of bounds");

  // Get the instruction's scheduling class
  const MCInstrDesc &Desc = MI.getDesc();
  unsigned SchedClass = Desc.getSchedClass();

  // Get operand cycle from itinerary
  // This tells us when the operand is read relative to instruction issue
  std::optional<unsigned> OperandCycle =
      Itin->getOperandCycle(SchedClass, OpIdx);

  // Ensure we have timing information for this operand
  assert(OperandCycle.has_value() &&
         "Itinerary must provide operand cycle information for all operands");

  return *OperandCycle;
}

unsigned AIEScheduleInterpreter::getOperandSubRegIdx(const MachineInstr &MI,
                                                     unsigned OpIdx) const {

  if (OpIdx >= MI.getNumOperands())
    return 0;

  const MachineOperand &MO = MI.getOperand(OpIdx);
  if (!MO.isReg())
    return 0;

  // Return the subregister index if present
  return MO.getSubReg();
}

// Helper to add an event to the schedule, resizing if necessary
static void addEvent(EventSchedule &Schedule, int Cycle, EventType Type,
                     unsigned VReg, unsigned SubRegIdx, const MachineInstr *MI,
                     unsigned OpIdx) {
  // Ensure the schedule is large enough
  if (Cycle >= static_cast<int>(Schedule.size())) {
    Schedule.resize(Cycle + 1);
  }

  // Add the event
  Schedule[Cycle].emplace_back(Type, VReg, SubRegIdx, MI, OpIdx);
}

void AIEScheduleInterpreter::addInstructionEvents(
    const MachineInstr &MI, int IssueCycle, EventSchedule &Schedule) const {

  LLVM_DEBUG(dbgs() << "Adding events for instruction at cycle " << IssueCycle
                    << ": " << MI);

  // Process all operands
  for (unsigned OpIdx = 0; OpIdx < MI.getNumOperands(); ++OpIdx) {
    const MachineOperand &MO = MI.getOperand(OpIdx);

    // Skip non-register operands
    if (!MO.isReg() || !MO.getReg())
      continue;

    // Skip physical registers for now
    if (!Register::isVirtualRegister(MO.getReg()))
      continue;

    // Skip implicit operands
    if (MO.isImplicit())
      continue;

    Register VReg = MO.getReg();
    unsigned SubRegIdx = getOperandSubRegIdx(MI, OpIdx);

    if (MO.isUse()) {
      int ReadCycleOffset = getOperandCycle(MI, OpIdx);
      int ReadCycle = IssueCycle + ReadCycleOffset;

      // Add read event
      addEvent(Schedule, ReadCycle, EventType::Read, VReg, SubRegIdx, &MI,
               OpIdx);

      LLVM_DEBUG(dbgs() << "  Read %vreg" << Register::virtReg2Index(VReg);
                 if (SubRegIdx) dbgs()
                 << ":" << TRI.getSubRegIndexName(SubRegIdx);
                 dbgs() << " at cycle " << ReadCycle << "\n");
    }

    if (MO.isDef()) {
      int WriteCycleOffset = getOperandCycle(MI, OpIdx);
      int WriteCycle = IssueCycle + WriteCycleOffset;

      // Add write event
      addEvent(Schedule, WriteCycle, EventType::Write, VReg, SubRegIdx, &MI,
               OpIdx);

      LLVM_DEBUG(dbgs() << "  Write %vreg" << Register::virtReg2Index(VReg);
                 if (SubRegIdx) dbgs()
                 << ":" << TRI.getSubRegIndexName(SubRegIdx);
                 dbgs() << " at cycle " << WriteCycle << "\n");
    }
  }
}

void AIEScheduleInterpreter::dumpEventSchedule(const EventSchedule &Schedule,
                                               raw_ostream &OS) const {

  // Collect all unique virtual registers
  std::set<unsigned> AllVRegs;
  for (const auto &CycleEvents : Schedule) {
    for (const auto &Event : CycleEvents) {
      AllVRegs.insert(Event.VReg);
    }
  }

  // Build a map of events per VReg
  std::map<unsigned, std::map<unsigned, std::string>> EventsByVReg;
  for (unsigned Cycle = 0; Cycle < Schedule.size(); ++Cycle) {
    const auto &CycleEvents = Schedule[Cycle];
    for (const auto &Event : CycleEvents) {
      char Action = (Event.Type == EventType::Read) ? 'R' : 'W';
      std::string ActionStr;
      if (Event.SubRegIdx != 0) {
        // Include subreg info if present (format as R## or W##)
        raw_string_ostream Stream(ActionStr);
        Stream << format("%c%02d", Action, Event.SubRegIdx);
      } else {
        // No subreg, just the action with padding
        ActionStr = Action;
        ActionStr += "  ";
      }
      // Add space if there's already an event in this cycle
      if (!EventsByVReg[Event.VReg][Cycle].empty()) {
        EventsByVReg[Event.VReg][Cycle] += " ";
      }
      EventsByVReg[Event.VReg][Cycle] += ActionStr;
    }
  }

  // Print header with cycle numbers
  OS << "VReg   |";
  for (unsigned Cycle = 0; Cycle < Schedule.size(); ++Cycle) {
    OS << format(" %4d |", Cycle);
  }
  OS << "\n";

  // Print separator
  OS << "-------+";
  for (unsigned Cycle = 0; Cycle < Schedule.size(); ++Cycle) {
    OS << "------+";
  }
  OS << "\n";

  // Print each VReg row
  for (unsigned VReg : AllVRegs) {
    OS << format("%6d |", Register::virtReg2Index(VReg));

    const auto &VRegEvents = EventsByVReg[VReg];
    for (unsigned Cycle = 0; Cycle < Schedule.size(); ++Cycle) {
      auto It = VRegEvents.find(Cycle);
      if (It != VRegEvents.end()) {
        OS << format(" %-4s |", It->second.c_str());
      } else {
        OS << "      |";
      }
    }
    OS << "\n";
  }
}

// Helper function to get lane mask for a register operand
static LaneBitmask getLaneMaskFor(const TargetRegisterInfo &TRI,
                                  const MachineRegisterInfo &MRI,
                                  unsigned SubRegIdx, unsigned VReg) {
  if (SubRegIdx == 0) {
    // Full/composite register - get the actual lane mask from register class
    const TargetRegisterClass *RC = MRI.getRegClass(VReg);
    return RC->getLaneMask();
  }
  // Specific subregister
  return TRI.getSubRegIndexLaneMask(SubRegIdx);
}

DenseMap<unsigned, AIE::LaneMaskVector>
AIEScheduleInterpreter::buildLiveLanes(const EventSchedule &Schedule,
                                       int II) const {

  assert(II > 0 && "Initiation interval must be positive");

  DenseMap<unsigned, AIE::LaneMaskVector> LiveLanesByVirtReg;

  if (Schedule.empty())
    return LiveLanesByVirtReg;

  // State: tracks which lanes are currently live when scanning backward
  DenseMap<unsigned /*VReg*/, LaneBitmask> ActiveMask;

  // Process cycles backward
  int MaxCycle = Schedule.size() - 1;
  for (int C = MaxCycle; C >= 0; --C) {
    const auto &Events = Schedule[C];
    int ModuloCycle = C % II; // Master modulo-II bit

    // First, record what's live ENTERING this cycle (before any events)
    // This is what was active from processing later cycles
    for (const auto &[VReg, Mask] : ActiveMask) {
      if (Mask.any()) {
        // Ensure the output vector is sized for this VReg
        if (!LiveLanesByVirtReg.count(VReg)) {
          LiveLanesByVirtReg[VReg] = AIE::LaneMaskVector(II);
        }
        LiveLanesByVirtReg[VReg][ModuloCycle] |= Mask;

        LLVM_DEBUG(dbgs() << "    Lanes " << PrintLaneMask(Mask) << " for %vreg"
                          << Register::virtReg2Index(VReg)
                          << " live entering cycle " << C << " (offset "
                          << ModuloCycle << ")\n");
      }
    }

    // Collect reads for this cycle (they don't make register live in this
    // cycle)
    DenseMap<unsigned /*VReg*/, LaneBitmask> ReadsInCycle;

    // Step 1: Process defs (writes) - they occupy the register and kill lanes
    // going backward
    for (const auto &Event : Events) {
      if (Event.Type == EventType::Write) {
        LaneBitmask M = getLaneMaskFor(TRI, MRI, Event.SubRegIdx, Event.VReg);

        // Ensure the output vector exists for this VReg
        if (!LiveLanesByVirtReg.count(Event.VReg)) {
          LiveLanesByVirtReg[Event.VReg] = AIE::LaneMaskVector(II);
        }

        // RF write occupies register file at ModuloCycle
        LiveLanesByVirtReg[Event.VReg][ModuloCycle] |= M;

        // Kill those lanes going backward
        ActiveMask[Event.VReg] &= ~M;

        LLVM_DEBUG(dbgs() << "  Cycle " << C << " (" << ModuloCycle
                          << "): Write %vreg"
                          << Register::virtReg2Index(Event.VReg);
                   if (Event.SubRegIdx) dbgs()
                   << ":" << TRI.getSubRegIndexName(Event.SubRegIdx);
                   dbgs() << " occupies lanes " << PrintLaneMask(M)
                          << " and kills them going backward\n");

        // If no lanes remain active, remove from map
        if (ActiveMask[Event.VReg].none()) {
          ActiveMask.erase(Event.VReg);
        }
      }
    }

    // Step 2: Collect all reads in this cycle
    for (const auto &Event : Events) {
      if (Event.Type == EventType::Read) {
        LaneBitmask M = getLaneMaskFor(TRI, MRI, Event.SubRegIdx, Event.VReg);

        // Accumulate reads for this VReg in this cycle
        ReadsInCycle[Event.VReg] |= M;

        LLVM_DEBUG(dbgs() << "  Cycle " << C << " (" << ModuloCycle
                          << "): Read %vreg"
                          << Register::virtReg2Index(Event.VReg);
                   if (Event.SubRegIdx) dbgs()
                   << ":" << TRI.getSubRegIndexName(Event.SubRegIdx);
                   dbgs() << " lanes " << PrintLaneMask(M) << "\n");
      }
    }

    // Step 3: Now propagate reads to ActiveMask for previous cycles
    // Reads don't make the register live in the current cycle
    for (const auto &[VReg, Mask] : ReadsInCycle) {
      // The reads make the register live going backward (but not in this cycle)
      ActiveMask[VReg] |= Mask;

      LLVM_DEBUG(dbgs() << "    %vreg" << Register::virtReg2Index(VReg)
                        << " lanes " << PrintLaneMask(Mask)
                        << " become live going backward from cycle " << C
                        << "\n");
    }
  }

  // At the end, ActiveMask should be empty (all defs should have been seen)
  // If not, we have uses without defs (which would be an error in def-first
  // semantics)
  for (const auto &[VReg, Mask] : ActiveMask) {
    if (Mask.any()) {
      LLVM_DEBUG(dbgs() << "Warning: %vreg" << Register::virtReg2Index(VReg)
                        << " has lanes " << PrintLaneMask(Mask)
                        << " live at beginning (use without def?)\n");
    }
  }

  return LiveLanesByVirtReg;
}

void AIEScheduleInterpreter::dumpLiveLanes(
    const DenseMap<unsigned, AIE::LaneMaskVector> &LiveLanesByVirtReg, int II,
    raw_ostream &OS) const {

  if (LiveLanesByVirtReg.empty()) {
    OS << "No live lanes data\n";
    return;
  }

  // Collect and sort VRegs for consistent output
  SmallVector<unsigned, 16> VRegs;
  for (const auto &[VReg, _] : LiveLanesByVirtReg) {
    VRegs.push_back(VReg);
  }
  llvm::sort(VRegs);

  OS << "Live Lanes (II=" << II << "):\n";
  OS << "VReg   | ";
  for (int T = 0; T < II; ++T) {
    OS << format("t%-2d ", T);
  }
  OS << "\n";

  OS << "-------+";
  for (int T = 0; T < II; ++T) {
    OS << "----";
  }
  OS << "\n";

  for (unsigned VReg : VRegs) {
    OS << format("%-6d | ", Register::virtReg2Index(VReg));

    const auto &LanesByOffset = LiveLanesByVirtReg.lookup(VReg);
    for (int T = 0; T < II; ++T) {
      LaneBitmask Mask = LanesByOffset[T];
      if (Mask.any()) {
        // Show a simple indicator - could be enhanced to show actual lanes
        OS << " ## ";
      } else {
        OS << " .. ";
      }
    }
    OS << "\n";
  }
}

BitVector
AIEScheduleInterpreter::buildSubRegBitmap(ArrayRef<LaneBitmask> LaneByOffset,
                                          unsigned SubRegIdx) const {

  int II = LaneByOffset.size();
  BitVector BV(II, false);

  LaneBitmask SubRegMask = (SubRegIdx == 0)
                               ? LaneBitmask::getAll()
                               : TRI.getSubRegIndexLaneMask(SubRegIdx);

  for (int T = 0; T < II; ++T) {
    BV[T] = (LaneByOffset[T] & SubRegMask).any();
  }

  return BV;
}

BitVector AIEScheduleInterpreter::buildVRegBitmap(
    ArrayRef<LaneBitmask> LaneByOffset) const {

  int II = LaneByOffset.size();
  BitVector BV(II, false);

  for (int T = 0; T < II; ++T) {
    BV[T] = LaneByOffset[T].any();
  }

  return BV;
}
