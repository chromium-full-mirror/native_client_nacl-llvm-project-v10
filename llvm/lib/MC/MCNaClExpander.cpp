//===- MCNaClExpander.cpp ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// This file was written by the Native Client authors.
//
//===----------------------------------------------------------------------===//
//
// This file implements the MCNaClExpander class. This is a base
// class that encapsulates the expansion logic for MCInsts, and holds
// state such as available scratch registers.
//
//===----------------------------------------------------------------------===//

#include "llvm/MC/MCNaClExpander.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"

namespace llvm {

void MCNaClExpander::Error(const MCInst &Inst, const char msg[]) {
  Ctx.reportFatalError(Inst.getLoc(), msg);
}

bool MCNaClExpander::addScratchReg(MCRegister Reg) {
  if (!isValidScratchRegister(Reg))
    return true;
  ScratchRegs.push_back(Reg);
  return false;
}

void MCNaClExpander::invalidateScratchRegs(const MCInst &Inst) {
  // TODO(dschuff): There are arch-specific special cases where this fails,
  // e.g. xchg/cmpxchg
  const MCInstrDesc &Desc = InstInfo->get(Inst.getOpcode());
  for (auto I = ScratchRegs.begin(), E = ScratchRegs.end(); I != E; ++I) {
    if (Desc.hasDefOfPhysReg(Inst, *I, *RegInfo))
      I = ScratchRegs.erase(I);
  }
}

void MCNaClExpander::clearScratchRegs() {
  ScratchRegs.clear();
}

MCRegister MCNaClExpander::getScratchReg(int index) {
  assert(index >= 0 && static_cast<unsigned>(index) < numScratchRegs());
  return ScratchRegs[numScratchRegs()  - index - 1];
}

unsigned MCNaClExpander::numScratchRegs() const { return ScratchRegs.size(); }

bool MCNaClExpander::isPseudo(const MCInst &Inst) const {
  return InstInfo->get(Inst.getOpcode()).isPseudo();
}

bool MCNaClExpander::mayAffectControlFlow(const MCInst &Inst) const {
  return InstInfo->get(Inst.getOpcode()).mayAffectControlFlow(Inst, *RegInfo);
}

bool MCNaClExpander::isCall(const MCInst &Inst) const {
  return InstInfo->get(Inst.getOpcode()).isCall();
}

bool MCNaClExpander::isBranch(const MCInst &Inst) const {
  return InstInfo->get(Inst.getOpcode()).isBranch();
}

bool MCNaClExpander::isIndirectBranch(const MCInst &Inst) const {
  return InstInfo->get(Inst.getOpcode()).isIndirectBranch();
}

bool MCNaClExpander::isReturn(const MCInst &Inst) const {
  return InstInfo->get(Inst.getOpcode()).isReturn();
}

bool MCNaClExpander::isVariadic(const MCInst &Inst) const {
  return InstInfo->get(Inst.getOpcode()).isVariadic();
}

bool MCNaClExpander::mayLoad(const MCInst &Inst) const {
  return InstInfo->get(Inst.getOpcode()).mayLoad();
}

bool MCNaClExpander::mayStore(const MCInst &Inst) const {
  return InstInfo->get(Inst.getOpcode()).mayStore();
}

bool MCNaClExpander::mayModifyRegister(const MCInst &Inst, MCRegister Reg) const {
  return InstInfo->get(Inst.getOpcode()).hasDefOfPhysReg(Inst, Reg, *RegInfo);
}

bool MCNaClExpander::explicitlyModifiesRegister(const MCInst &Inst,
                                                MCRegister Reg) const {
  const MCInstrDesc &Desc = InstInfo->get(Inst.getOpcode());
  for (int i = 0; i < Desc.NumDefs; ++i) {
    if (Desc.OpInfo[i].OperandType == MCOI::OPERAND_REGISTER &&
        RegInfo->isSubRegisterEq(Reg, Inst.getOperand(i).getReg()))
      return true;
  }
  return false;
}

// Replaces all definitions of RegOld with RegNew in Inst
void MCNaClExpander::replaceDefinitions(MCInst &Inst, MCRegister RegOld,
                                        MCRegister RegNew) const {
  const MCInstrDesc &Desc = InstInfo->get(Inst.getOpcode());
  for (int i = 0; i < Desc.NumDefs; ++i) {
    MCOperand &Op = Inst.getOperand(i);
    if (Op.isReg() && Op.getReg() == RegOld)
      Op.setReg(RegNew);
  }
}
}
