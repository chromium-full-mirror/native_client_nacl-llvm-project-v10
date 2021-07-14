//===- ARMMCNaClExpander.cpp ------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the ARMMCNaClExpander class, the ARM specific
// subclass of MCNaClExpander.
//
//===----------------------------------------------------------------------===//
#include "ARMMCNaClExpander.h"
#include "ARMAddressingModes.h"
#include "MCTargetDesc/ARMBaseInfo.h"

#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrDesc.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCNaClExpander.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCStreamer.h"

using namespace llvm;

const unsigned kBranchTargetMask = 0xC000000F;
const unsigned kSandboxMask = 0xC0000000;

bool ARM::ARMMCNaClExpander::isValidScratchRegister(MCRegister Reg) const {
  return Reg != ARM::PC && Reg != ARM::SP;
}

static void emitBicMask(unsigned Mask, MCRegister Reg, ARMCC::CondCodes Pred,
                        MCRegister PredReg, MCStreamer &Out,
                        const MCSubtargetInfo &STI) {
  MCInst Bic;
  const int32_t EncodedMask = ARM_AM::getSOImmVal(Mask);
  Bic.setOpcode(ARM::BICri);
  Bic.addOperand(MCOperand::createReg(Reg));
  Bic.addOperand(MCOperand::createReg(Reg));
  Bic.addOperand(MCOperand::createImm(EncodedMask));
  Bic.addOperand(MCOperand::createImm(Pred));
  Bic.addOperand(MCOperand::createReg(PredReg));
  Bic.addOperand(MCOperand::createReg(0));
  Out.emitInstruction(Bic, STI);
}

static ARMCC::CondCodes
getPredicate(const MCInst &Inst, const MCInstrInfo &Info, MCRegister &PredReg) {
  const MCInstrDesc &Desc = Info.get(Inst.getOpcode());
  int PIdx = Desc.findFirstPredOperandIdx();
  if (PIdx == -1) {
    PredReg = 0;
    return ARMCC::AL;
  }

  PredReg = Inst.getOperand(PIdx + 1).getReg();
  return static_cast<ARMCC::CondCodes>(Inst.getOperand(PIdx).getImm());
}

// return a conditional branch through Reg based on the condition codes of Inst
static MCInst getConditionalBranch(MCRegister Reg, const MCInst &Inst,
                                   const MCInstrInfo &II) {
  MCRegister PredReg;
  ARMCC::CondCodes Pred = getPredicate(Inst, II, PredReg);

  MCInst BranchInst;
  BranchInst.setOpcode(ARM::BX_pred);
  BranchInst.addOperand(MCOperand::createReg(Reg));
  BranchInst.addOperand(MCOperand::createImm(Pred));
  BranchInst.addOperand(MCOperand::createReg(PredReg));
  return BranchInst;
}

void ARM::ARMMCNaClExpander::expandIndirectBranch(const MCInst &Inst,
                                                  MCStreamer &Out,
                                                  const MCSubtargetInfo &STI,
                                                  bool isCall) {
  assert(Inst.getOperand(0).isReg());
  MCRegister BranchReg = Inst.getOperand(0).getReg();

  // No need to sandbox branch through sp or pc
  if (BranchReg == ARM::SP || BranchReg == ARM::PC)
    return Out.emitInstruction(Inst, STI);

  MCRegister PredReg;
  ARMCC::CondCodes Pred = getPredicate(Inst, *InstInfo, PredReg);

  // Otherwise, mask target and branch through
  Out.emitBundleLock(isCall);
  emitBicMask(kBranchTargetMask, BranchReg, Pred, PredReg, Out, STI);
  Out.emitInstruction(Inst, STI);
  Out.emitBundleUnlock();
}

void ARM::ARMMCNaClExpander::expandCall(const MCInst &Inst, MCStreamer &Out,
                                        const MCSubtargetInfo &STI) {
  // Test for indirect call
  if (Inst.getOperand(0).isReg()) {
    expandIndirectBranch(Inst, Out, STI, true);
  }

  // Otherwise, we are a direct call, so just emit
  else {
    Out.emitInstruction(Inst, STI);
  }
}

void ARM::ARMMCNaClExpander::expandReturn(const MCInst &Inst, MCStreamer &Out,
                                          const MCSubtargetInfo &STI) {
  unsigned Opcode = Inst.getOpcode();
  if (Opcode == ARM::BX_RET || Opcode == ARM::MOVPCLR) {
    MCInst BranchInst = getConditionalBranch(ARM::LR, Inst, *InstInfo);
    return expandIndirectBranch(BranchInst, Out, STI, false);
  }

  return Out.emitInstruction(Inst, STI);
}

void ARM::ARMMCNaClExpander::expandControlFlow(const MCInst &Inst,
                                               MCStreamer &Out,
                                               const MCSubtargetInfo &STI) {

  // Optimize if we are just moving into PC
  if (Inst.getOpcode() == ARM::MOVr && Inst.getOperand(0).getReg() == ARM::PC) {
    MCRegister Src = Inst.getOperand(1).getReg();
    MCInst BranchInst = getConditionalBranch(Src, Inst, *InstInfo);
    return expandIndirectBranch(BranchInst, Out, STI, false);
  }

  MCRegister Scratch;
  if (numScratchRegs() == 0) {
    Scratch = ARM::R12;
  } else {
    Scratch = getScratchReg(0);
  }

  MCInst SandboxedInst(Inst);

  replaceDefinitions(SandboxedInst, ARM::PC, Scratch);
  doExpandInst(SandboxedInst, Out, STI);

  MCInst BranchInst = getConditionalBranch(Scratch, Inst, *InstInfo);
  doExpandInst(BranchInst, Out, STI);
}

bool ARM::ARMMCNaClExpander::mayModifyStack(const MCInst &Inst) {
  // No way to tell where the variable reglist starts, so conservatively
  // check all registers if the instruction is a variadic load like LDM/VLDM
  if (isVariadic(Inst) && mayLoad(Inst)) {
    for (unsigned i = 0, e = Inst.getNumOperands(); i != e; ++i) {
      if (Inst.getOperand(i).isReg() && Inst.getOperand(i).getReg() == ARM::SP)
        return true;
    }
  }
  // Otherwise, check if any definitions are SP
  return mayModifyRegister(Inst, ARM::SP);
}

void ARM::ARMMCNaClExpander::expandStackManipulation(
    const MCInst &Inst, MCStreamer &Out, const MCSubtargetInfo &STI) {

  // Dont sandbox push/pop
  switch (Inst.getOpcode()) {
  case ARM::LDMIA_UPD:
  case ARM::LDMIB_UPD:
  case ARM::LDMDA_UPD:
  case ARM::LDMDB_UPD:
  case ARM::VLDMDIA_UPD:
  case ARM::VLDMDDB_UPD:
  case ARM::VLDMSIA_UPD:
  case ARM::VLDMSDB_UPD:
  case ARM::STMIA_UPD:
  case ARM::STMIB_UPD:
  case ARM::STMDA_UPD:
  case ARM::STMDB_UPD:
  case ARM::VSTMDIA_UPD:
  case ARM::VSTMDDB_UPD:
  case ARM::VSTMSIA_UPD:
  case ARM::VSTMSDB_UPD:
  case ARM::LDR_PRE_IMM:
  case ARM::STR_PRE_IMM:
    if (Inst.getOperand(0).getReg() == ARM::SP)
      return Out.emitInstruction(Inst, STI);
    break;
  case ARM::LDR_POST_IMM:
  case ARM::STR_POST_IMM:
    if (Inst.getOperand(1).getReg() == ARM::SP)
      return Out.emitInstruction(Inst, STI);
    break;
  default:
    break;
  }

  MCRegister PredReg;
  ARMCC::CondCodes Pred = getPredicate(Inst, *InstInfo, PredReg);

  Out.emitBundleLock(false);
  if (mayLoad(Inst) || mayStore(Inst)) {
    expandLoadStore(Inst, Out, STI);
  } else {
    Out.emitInstruction(Inst, STI);
  }
  emitBicMask(kSandboxMask, ARM::SP, Pred, PredReg, Out, STI);
  Out.emitBundleUnlock();
}

static int getMemIdx(const MCInst &Inst, const MCInstrInfo &InstInfo) {
  unsigned Opc = Inst.getOpcode();
  const MCOperandInfo *OpInfo = InstInfo.get(Opc).OpInfo;
  for (int i = 0, e = Inst.getNumOperands(); i < e; i++) {
    if (OpInfo[i].OperandType == MCOI::OPERAND_MEMORY) {
      return i;
    }
  }
  return -1;
}

// Sandbox an instruction that uses simple base + imm displacement
// addressing mode.
static void sandboxBaseDisp(const MCInst &Inst, const MCInstrInfo &II,
                            MCRegister BaseReg, MCStreamer &Out,
                            const MCSubtargetInfo &STI) {
  switch (BaseReg) {
  case ARM::PC:
  case ARM::SP:
    return Out.emitInstruction(Inst, STI);
  }

  MCRegister PredReg;
  ARMCC::CondCodes Pred = getPredicate(Inst, II, PredReg);

  Out.emitBundleLock(false);
  emitBicMask(kSandboxMask, BaseReg, Pred, PredReg, Out, STI);
  Out.emitInstruction(Inst, STI);
  Out.emitBundleUnlock();
}

// Create an ADD instruction which computes the base + reg [+ scale]
// addressing mode in base LDR/STR instructions into the register Target.
static MCInst getAddrInstr(const MCInst &Inst, const MCInstrInfo &II,
                           int MemIdx, MCRegister Target) {
  assert(Inst.getOperand(MemIdx).isReg());

  unsigned AM2Opc = Inst.getOperand(MemIdx + 2).getImm();
  unsigned Offset = ARM_AM::getAM2Offset(AM2Opc);
  ARM_AM::ShiftOpc ShOp = ARM_AM::getSORegShOp(ARM_AM::getAM2ShiftOpc(AM2Opc));

  MCRegister PredReg;
  ARMCC::CondCodes Pred = getPredicate(Inst, II, PredReg);
  MCInst Add;

  // An ADDrsi op with a shift type of no_shift is equivalent to an ADDrr op;
  // the asm printer does not care about the distinction and prints it correctly
  // but the object file encoder asserts that the shift type of ADDrsi isn't
  // no_shift. So create ADDrr when applicable.
  Add.setOpcode(ShOp == ARM_AM::no_shift ? ARM::ADDrr : ARM::ADDrsi);
  Add.addOperand(MCOperand::createReg(Target));
  Add.addOperand(Inst.getOperand(MemIdx));
  Add.addOperand(Inst.getOperand(MemIdx + 1));
  if (ShOp != ARM_AM::no_shift)
    Add.addOperand(MCOperand::createImm(ARM_AM::getSORegOpc(ShOp, Offset)));
  Add.addOperand(MCOperand::createImm(Pred));
  Add.addOperand(MCOperand::createReg(PredReg));
  Add.addOperand(MCOperand::createReg(0));
  return Add;
}

// Demote the load/store opcode to the sandboxed equivalent, i.e.,
// the version that uses a base + immediate displacement.
// TODO: add support for different sizes, like LDRB, LDRH, etc
static unsigned sandboxOpcode(unsigned Opcode) {
  switch (Opcode) {
  default:
    return Opcode;
  case ARM::LDRi12:
  case ARM::LDR_PRE_IMM:
  case ARM::LDR_PRE_REG:
  case ARM::LDRrs:
    return ARM::LDRi12;
  case ARM::STRi12:
  case ARM::STR_PRE_IMM:
  case ARM::STR_PRE_REG:
  case ARM::STRrs:
    return ARM::STRi12;
  }
}

// Sandbox the load/store with reg + reg + shift displacement into a
// simple load/store with base + imediate displacement.  Target is the
// register that will eventually be the base in the sandboxed
// load/store.  This is useful for passing in a scratch register, or
// the destination operand (for loads). RegIdx is the index of the
// operand which is the register to load to/store from, and MemIdx
// is the index to the memory operand
static void sandboxBaseRegScale(const MCInst &Inst, const MCInstrInfo &II,
                                bool PostIncrement, int RegIdx, int MemIdx,
                                MCRegister Target, MCStreamer &Out,
                                const MCSubtargetInfo &STI) {
  MCRegister PredReg;
  ARMCC::CondCodes Pred = getPredicate(Inst, II, PredReg);

  if (PostIncrement) {
    sandboxBaseDisp(Inst, II, Inst.getOperand(MemIdx).getReg(), Out, STI);
  } else {
    Out.emitBundleLock(false);
    Out.emitInstruction(getAddrInstr(Inst, II, MemIdx, Target), STI);
    emitBicMask(kSandboxMask, Target, Pred, PredReg, Out, STI);

    MCInst SandboxedInst;
    SandboxedInst.setOpcode(sandboxOpcode(Inst.getOpcode()));
    SandboxedInst.addOperand(Inst.getOperand(RegIdx));
    SandboxedInst.addOperand(MCOperand::createReg(Target));
    SandboxedInst.addOperand(MCOperand::createImm(0));
    SandboxedInst.addOperand(MCOperand::createImm(Pred));
    SandboxedInst.addOperand(MCOperand::createReg(PredReg));
    Out.emitInstruction(SandboxedInst, STI);
    Out.emitBundleUnlock();
  }
}

void ARM::ARMMCNaClExpander::expandPrefetch(const MCInst &Inst, MCStreamer &Out,
                                            const MCSubtargetInfo &STI) {
  if (Inst.getOpcode() == ARM::PLDi12) {
    return sandboxBaseDisp(Inst, *InstInfo, Inst.getOperand(0).getReg(), Out,
                           STI);
  } else if (Inst.getOpcode() == ARM::PLDrs) {
    MCRegister Scratch;
    if (numScratchRegs() == 0) {
      Scratch = ARM::R12;
    } else {
      Scratch = getScratchReg(0);
    }

    Out.emitBundleLock(false);
    Out.emitInstruction(getAddrInstr(Inst, *InstInfo, 0, Scratch), STI);

    MCRegister PredReg;
    ARMCC::CondCodes Pred = getPredicate(Inst, *InstInfo, PredReg);

    emitBicMask(kSandboxMask, Scratch, Pred, PredReg, Out, STI);

    MCInst SandboxedInst;
    SandboxedInst.setOpcode(ARM::PLDi12);
    SandboxedInst.addOperand(MCOperand::createReg(Scratch));
    SandboxedInst.addOperand(MCOperand::createImm(0));
    Out.emitInstruction(SandboxedInst, STI);

    Out.emitBundleUnlock();
  } else {
    Out.emitInstruction(Inst, STI);
  }
}

void ARM::ARMMCNaClExpander::expandLoadStore(const MCInst &Inst,
                                             MCStreamer &Out,
                                             const MCSubtargetInfo &STI) {
  switch (Inst.getOpcode()) {
  case ARM::STMIA_UPD:
  case ARM::STMDA_UPD:
  case ARM::STMDB_UPD:
  case ARM::STMIB_UPD:

  case ARM::VSTMDIA:
  case ARM::VSTMDIA_UPD:
  case ARM::VSTMDDB_UPD:
  case ARM::VSTMSIA:
  case ARM::VSTMSIA_UPD:
  case ARM::VSTMSDB_UPD:

  case ARM::LDMIA_UPD:
  case ARM::LDMDA_UPD:
  case ARM::LDMDB_UPD:
  case ARM::LDMIB_UPD:

  case ARM::VLDMDIA:
  case ARM::VLDMDIA_UPD:
  case ARM::VLDMDDB_UPD:
  case ARM::VLDMSIA:
  case ARM::VLDMSIA_UPD:
  case ARM::VLDMSDB_UPD:

  case ARM::STMIA:
  case ARM::STMDA:
  case ARM::STMDB:
  case ARM::STMIB:

  case ARM::LDMIA:
  case ARM::LDMDA:
  case ARM::LDMDB:
  case ARM::LDMIB:
    return sandboxBaseDisp(Inst, *InstInfo, Inst.getOperand(0).getReg(), Out,
                           STI);
  }

  int MemIdx = getMemIdx(Inst, *InstInfo);
  // Some instructions have the mayLoad/mayStore bits but no memory operands,
  // e.g. DMB, or have expression operands (e.g. LDR with a label operand or
  // ADR). If there are no memory operands, or the memory operand is not a reg,
  // don't modify the instruction.
  if (MemIdx == -1 || Inst.getOperand(MemIdx).isExpr())
    return Out.emitInstruction(Inst, STI);

  MCRegister BaseReg = Inst.getOperand(MemIdx).getReg();
  MCRegister Scratch;
  bool PostIncrement = false;

  switch (Inst.getOpcode()) {
  case ARM::PLDi12:
  case ARM::PLDrs:
    return expandPrefetch(Inst, Out, STI);
  case ARM::LDRi12:
  case ARM::STRi12:
  case ARM::LDR_PRE_IMM:
  case ARM::STR_PRE_IMM:
  case ARM::LDR_POST_IMM:
  case ARM::STR_POST_IMM:
    return sandboxBaseDisp(Inst, *InstInfo, Inst.getOperand(MemIdx).getReg(),
                           Out, STI);
  case ARM::LDR_PRE_REG:
    return sandboxBaseRegScale(Inst, *InstInfo, false, 0, MemIdx, BaseReg, Out,
                               STI);
  case ARM::STR_PRE_REG:
    return sandboxBaseRegScale(Inst, *InstInfo, false, 1, MemIdx, BaseReg, Out,
                               STI);
  case ARM::LDR_POST_REG:
    PostIncrement = true;
    [[fallthrough]];
  case ARM::LDRrs:
    return sandboxBaseRegScale(Inst, *InstInfo, PostIncrement, 0, MemIdx,
                               Inst.getOperand(0).getReg(), Out, STI);
  case ARM::STR_POST_REG:
    PostIncrement = true;
    [[fallthrough]];
  case ARM::STRrs:
    if (numScratchRegs() == 0) {
      Scratch = ARM::R12;
    } else {
      Scratch = getScratchReg(0);
    }
    return sandboxBaseRegScale(Inst, *InstInfo, PostIncrement, 0, MemIdx,
                               Scratch, Out, STI);
  default:
    // Fall back case, should handle all other instructions that load/store
    // memory such as VFP/NEON loads/stores and prefetch instructions.
    return sandboxBaseDisp(Inst, *InstInfo, Inst.getOperand(MemIdx).getReg(),
                           Out, STI);
  }
}

void ARM::ARMMCNaClExpander::doExpandInst(const MCInst &Inst, MCStreamer &Out,
                                          const MCSubtargetInfo &STI) {
  if (isReturn(Inst))
    return expandReturn(Inst, Out, STI);

  if (isIndirectBranch(Inst))
    return expandIndirectBranch(Inst, Out, STI, false);

  if (isCall(Inst))
    return expandCall(Inst, Out, STI);

  if (isBranch(Inst))
    return Out.emitInstruction(Inst, STI);

  if (mayAffectControlFlow(Inst))
    return expandControlFlow(Inst, Out, STI);

  if (mayModifyStack(Inst))
    return expandStackManipulation(Inst, Out, STI);

  if (mayLoad(Inst) || mayStore(Inst))
    return expandLoadStore(Inst, Out, STI);

  return Out.emitInstruction(Inst, STI);
}

bool ARM::ARMMCNaClExpander::expandInst(const MCInst &Inst, MCStreamer &Out,
                                        const MCSubtargetInfo &STI) {
  if (Guard)
    return false;
  Guard = true;

  doExpandInst(Inst, Out, STI);

  Guard = false;
  return true;
}
