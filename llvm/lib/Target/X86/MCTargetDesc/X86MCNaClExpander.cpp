//===- X86MCNaClExpander.cpp - X86 NaCl Expander --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the X86MCNaClExpander class, the X86 specific
// subclass of MCNaClExpander.
//
// This file was written by the Native Client authors.
//
//===----------------------------------------------------------------------===//
#include "X86MCNaClExpander.h"
#include "X86BaseInfo.h"

#include "X86MCTargetDesc.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrDesc.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCNaClExpander.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "saigo"

static const int kBundleSize = 32;
// 2020 notes this used to be defined as a build flag for performance testing.
// For now, just setting it to its default.
// extern cl::opt<bool> FlagHideSandboxBase;
static bool FlagHideSandboxBase = true;

cl::opt<bool> FlagUseX86CallConvention(
    "nacl-use-x86-call-convention",
    cl::desc("Use ECX/R11 as scratch registers for ret instructions. This is"
             " safe as long as the code respects standard x86 calling"
             " conventions, which specify these registers as scratch registers"
             " for a return."),
    cl::init(true));

namespace {

MCRegister getReg32(MCRegister Reg) {
  switch (Reg) {
  default:
    return getX86SubSuperRegisterOrZero(Reg, 32, false);
  case X86::IP:
  case X86::EIP:
    return X86::EIP;
  case X86::RIP:
    llvm_unreachable("Trying to demote %rip");
  }
}

MCRegister getReg64(MCRegister Reg) {
  switch (Reg) {
  default:
    return getX86SubSuperRegisterOrZero(Reg, 64, false);
  case X86::IP:
  case X86::EIP:
  case X86::RIP:
    return X86::RIP;
  }
}

bool isDirectCall(const MCInst &Inst) {
  switch (Inst.getOpcode()) {
    case X86::CALLpcrel32:
    case X86::CALL64pcrel32:
      return true;
    default:
      return false;
  }
}

bool isValidReturnRegister(const MCRegister& Reg) {
  return Reg == X86::EAX ||
         Reg == X86::RAX ||
         Reg == X86::AL ||
         Reg == X86::AX ||
         Reg == X86::XMM0;
}

bool isHighReg(MCRegister Reg) {
  return Reg == X86::AH || Reg == X86::BH || Reg == X86::CH || Reg == X86::DH;
}

// Detects movl %esp %ebp or movl %ebp %esp.
bool is32BitStackPtrToFramePtrMove(const MCInst &Inst) {
  return Inst.getOpcode() == X86::MOV32rr &&
         Inst.getNumOperands() == 2 &&
         Inst.getOperand(0).isReg() &&
         Inst.getOperand(1).isReg() &&
         ((Inst.getOperand(0).getReg() == X86::EBP &&
           Inst.getOperand(1).getReg() == X86::ESP) ||
          (Inst.getOperand(0).getReg() == X86::ESP &&
           Inst.getOperand(1).getReg() == X86::EBP));
}

// Detects push %rsp and push %rbp.
bool isStackRegPush(const MCInst &Inst) {
  return Inst.getOpcode() == X86::PUSH64r &&
         Inst.getNumOperands() == 1 &&
         Inst.getOperand(0).isReg() &&
         (Inst.getOperand(0).getReg() == X86::RBP ||
          Inst.getOperand(0).getReg() == X86::RSP);
}

} // namespace

static unsigned demoteOpcode(unsigned Reg);

static bool isAbsoluteReg(MCRegister Reg) {
  Reg = getReg64(Reg); // Normalize to 64 bits
  return (Reg == X86::R15 || Reg == X86::RSP || Reg == X86::RBP ||
          Reg == X86::RIP);
}

bool X86::X86MCNaClExpander::isValidScratchRegister(MCRegister Reg) const {
  if (isAbsoluteReg(Reg))
    return false;
  return true;
}


static void PushReturnAddress(const llvm::MCSubtargetInfo &STI,
                              MCContext &Context, MCStreamer &Out,
                              MCSymbol *RetTarget) {
  const MCExpr *RetTargetExpr = MCSymbolRefExpr::create(RetTarget, Context);
  if (Context.getObjectFileInfo()->isPositionIndependent()) {
    // Calculate return_addr
    // The return address should not be calculated into R11 because if the push
    // instruction ends up at the start of a bundle, an attacker could arrange
    // an indirect jump to it, which would push the full jump target
    // (which itself was calculated into r11) onto the stack.
    MCInst LEAInst;
    LEAInst.setOpcode(X86::LEA64_32r);
    LEAInst.addOperand(MCOperand::createReg(X86::R10D));      // DestReg
    LEAInst.addOperand(MCOperand::createReg(X86::RIP));       // BaseReg
    LEAInst.addOperand(MCOperand::createImm(1));              // Scale
    LEAInst.addOperand(MCOperand::createReg(0));              // IndexReg
    LEAInst.addOperand(MCOperand::createExpr(RetTargetExpr)); // Offset
    LEAInst.addOperand(MCOperand::createReg(0));              // SegmentReg
    Out.emitInstruction(LEAInst, STI);
    // push return_addr
    MCInst PUSHInst;
    PUSHInst.setOpcode(X86::PUSH64r);
    PUSHInst.addOperand(MCOperand::createReg(X86::R10));
    Out.emitInstruction(PUSHInst, STI);
  } else {
    // push return_addr
    MCInst PUSHInst;
    PUSHInst.setOpcode(X86::PUSH64i32);
    PUSHInst.addOperand(MCOperand::createExpr(RetTargetExpr));
    Out.emitInstruction(PUSHInst, STI);
  }
}

void X86::X86MCNaClExpander::emitSandboxBranchReg(MCRegister Reg, MCStreamer &Out,
                                                  const MCSubtargetInfo &STI) {

  MCInst AndInst;
  AndInst.setOpcode(X86::AND32ri8);

  MCOperand Target32 = MCOperand::createReg(getReg32(Reg));
  AndInst.addOperand(Target32);
  AndInst.addOperand(Target32);
  AndInst.addOperand(MCOperand::createImm(-kBundleSize));
  Out.emitInstruction(AndInst, STI);

  if (Is64Bit) {
    MCInst Add;
    Add.setOpcode(X86::ADD64rr);

    MCOperand Target64 = MCOperand::createReg(getReg64(Reg));
    Add.addOperand(Target64);
    Add.addOperand(Target64);
    Add.addOperand(MCOperand::createReg(X86::R15));
    Out.emitInstruction(Add, STI);
  }
}

void X86::X86MCNaClExpander::expandDirectCall(const MCInst &Inst,
                                              MCStreamer &Out,
                                              const MCSubtargetInfo &STI) {
  const bool HideSandboxBase = FlagHideSandboxBase && Is64Bit;
  if (HideSandboxBase) {
    // For NaCl64, the sequence
    //   call target
    //   return_addr:
    // is changed to
    //   push return_addr
    //   jmp target
    //   .align 32
    //   return_addr:
    // This avoids exposing the sandbox base address via the return
    // address on the stack.

    MCContext &Context = Out.getContext();

    // Generate a label for the return address.
    MCSymbol *RetTarget = Context.createTempSymbol("DirectCallRetAddr", true);

    PushReturnAddress(STI, Context, Out, RetTarget);

    // jmp target
    MCInst JMPInst;
    JMPInst.setOpcode(X86::JMP_4);
    JMPInst.addOperand(Inst.getOperand(0));
    Out.emitInstruction(JMPInst, STI);

    Out.emitCodeAlignment(kBundleSize);
    Out.emitLabel(RetTarget);
  } else {
    // Emit a nop in case the call is the first instruction of the function.
    // Otherwise, the function label would lose its alignment as it is moved
    // together with the call instruction.
    MCInst NOPInst;
    NOPInst.setOpcode(X86::NOOP);
    Out.emitInstruction(NOPInst, STI);
    Out.emitBundleLock(true);

    MCInst CALLInst;
    CALLInst.setOpcode(Is64Bit ? X86::CALL64pcrel32 : X86::CALLpcrel32);
    CALLInst.addOperand(Inst.getOperand(0));
    Out.emitInstruction(CALLInst, STI);
    Out.emitBundleUnlock();
  }
}

void X86::X86MCNaClExpander::emitIndirectJumpReg(MCRegister Reg, MCStreamer &Out,
                                                 const MCSubtargetInfo &STI) {

  Out.emitBundleLock(false);
  emitSandboxBranchReg(Reg, Out, STI);

  MCInst Jmp;
  Jmp.setOpcode(Is64Bit ? X86::JMP64r : X86::JMP32r);

  MCOperand Target =
      MCOperand::createReg(Is64Bit ? getReg64(Reg) : getReg32(Reg));
  Jmp.addOperand(Target);

  Out.emitInstruction(Jmp, STI);
  Out.emitBundleUnlock();
}

void X86::X86MCNaClExpander::emitIndirectCallReg(MCRegister Reg, MCStreamer &Out,
                                                 const MCSubtargetInfo &STI) {

  MCSymbol *RetTarget;
  MCContext &Context = Out.getContext();
  MCOperand Target =
      MCOperand::createReg(Is64Bit ? getReg64(Reg) : getReg32(Reg));

  if (Is64Bit && FlagHideSandboxBase) {
    // Generate a label for the return address.
    RetTarget = Context.createTempSymbol("IndirectCallRetAddr", true);
    PushReturnAddress(STI, Context, Out, RetTarget);

    Out.emitBundleLock(false);
    emitSandboxBranchReg(Reg, Out, STI);
    MCInst Jmp;
    Jmp.setOpcode(X86::JMP64r);
    Jmp.addOperand(Target);
    Out.emitInstruction(Jmp, STI);
    Out.emitBundleUnlock();

    Out.emitCodeAlignment(kBundleSize);
    Out.emitLabel(RetTarget);
  } else {
    // Emit a nop in case the call is the first instruction of the function.
    // Otherwise, the function label would lose its alignment as it is moved
    // together with the following instructions.
    MCInst NOPInst;
    NOPInst.setOpcode(X86::NOOP);
    Out.emitInstruction(NOPInst, STI);

    Out.emitBundleLock(true);
    emitSandboxBranchReg(Reg, Out, STI);
    MCInst Call;
    Call.setOpcode(Is64Bit ? X86::CALL64r : X86::CALL32r);
    Call.addOperand(Target);
    Out.emitInstruction(Call, STI);
    Out.emitBundleUnlock();
  }
}

void X86::X86MCNaClExpander::expandIndirectBranch(const MCInst &Inst,
                                                  MCStreamer &Out,
                                                  const MCSubtargetInfo &STI) {

  MCRegister Target;
  if (mayLoad(Inst)) {
    // indirect jmp/call through memory
    MCInst Mov;
    Mov.setOpcode(Is64Bit ? X86::MOV64rm : X86::MOV32rm);

    if (Is64Bit && FlagHideSandboxBase) {
      Target = X86::R11;
    } else if (FlagUseX86CallConvention && isCall(Inst)) {
      Target = Is64Bit ? X86::RCX : X86::ECX;
    } else if (numScratchRegs()) {
      Target = getScratchReg(0);
    } else {
      LLVM_DEBUG(Inst.dump());
      if (isCall(Inst)) {
        Error(Inst,
              "Not enough scratch registers when expanding indirect call.");
      } else {
        Error(Inst,
              "Not enough scratch registers when expanding indirect jump.");
      }
    }

    Mov.addOperand(
        MCOperand::createReg(Is64Bit ? getReg64(Target) : getReg32(Target)));
    Mov.addOperand(Inst.getOperand(0));
    Mov.addOperand(Inst.getOperand(1));
    Mov.addOperand(Inst.getOperand(2));
    Mov.addOperand(Inst.getOperand(3));
    Mov.addOperand(Inst.getOperand(4));
    doExpandInst(Mov, Out, STI, false);
  } else if (Is64Bit && FlagHideSandboxBase) {
    Target = X86::R11;
    MCRegister TargetOrig = getReg64(Inst.getOperand(0).getReg());
    if (TargetOrig != X86::R11) {
      MCInst Mov;
      Mov.setOpcode(X86::MOV64rr);
      Mov.addOperand(MCOperand::createReg(X86::R11));
      Mov.addOperand(MCOperand::createReg(TargetOrig));
      doExpandInst(Mov, Out, STI, false);
    }
  } else {
    Target = Inst.getOperand(0).getReg();
  }

  if (isCall(Inst))
    emitIndirectCallReg(Target, Out, STI);
  else
    emitIndirectJumpReg(Target, Out, STI);
}

void X86::X86MCNaClExpander::expandReturn(const MCInst &Inst, MCStreamer &Out,
                                          const MCSubtargetInfo &STI) {
  MCRegister ScratchReg;
  if (FlagUseX86CallConvention) {
    ScratchReg = Is64Bit ? X86::R11 : X86::ECX;
  } else {
    if (numScratchRegs() == 0)
      Error(Inst, "No scratch registers specified when expanding return.");

    ScratchReg = getScratchReg(0);
  }

  MCInst Pop;
  Pop.setOpcode(Is64Bit ? X86::POP64r : X86::POP32r);
  Pop.addOperand(MCOperand::createReg(Is64Bit ? getReg64(ScratchReg)
                                              : getReg32(ScratchReg)));
  Out.emitInstruction(Pop, STI);

  if (Inst.getNumOperands() > 0) {
    if (Inst.getOpcode() == X86::RETIL || Inst.getOpcode() == X86::RETIQ) {
      MCOperand StackPointer =
        MCOperand::createReg(Is64Bit ? X86::RSP : X86::ESP);

      MCInst Add;
      Add.setOpcode(Is64Bit ? X86::ADD64ri32 : X86::ADD32ri);
      Add.addOperand(StackPointer);
      Add.addOperand(StackPointer);
      Add.addOperand(Inst.getOperand(0));

      doExpandInst(Add, Out, STI, false);
    } else if (Inst.getOpcode() == X86::RETL || Inst.getOpcode() == X86::RETQ) {
      LLVM_DEBUG(Inst.dump());
      assert(Inst.getOperand(0).isReg());
      assert(isValidReturnRegister(Inst.getOperand(0).getReg()));
    } else {
      LLVM_DEBUG(Inst.dump());
      Error(Inst, "Unexpected return instruction.");
    }
  }

  emitIndirectJumpReg(ScratchReg, Out, STI);
}

// Emits movl Reg32, Reg32
// Used as a helper in various places.
static void clearHighBits(const MCOperand &Reg, MCStreamer &Out,
                          const MCSubtargetInfo &STI) {
  MCInst Mov;
  Mov.setOpcode(X86::MOV32rr);
  MCOperand Op = MCOperand::createReg(getReg32(Reg.getReg()));

  Mov.addOperand(Op);
  Mov.addOperand(Op);
  Out.emitInstruction(Mov, STI);
}

// Emits the sandboxing operations necessary, and modifies the memory
// operand specified by MemIdx.
// Used as a helper function for emitSandboxMemOps.
void X86::X86MCNaClExpander::emitSandboxMemOp(MCInst &Inst, int MemIdx,
                                              MCRegister ScratchReg,
                                              MCStreamer &Out,
                                              const MCSubtargetInfo &STI) {
  MCOperand &Base = Inst.getOperand(MemIdx);
  MCOperand &Scale = Inst.getOperand(MemIdx + 1);
  MCOperand &Index = Inst.getOperand(MemIdx + 2);
  MCOperand &Offset = Inst.getOperand(MemIdx + 3);
  MCOperand &Segment = Inst.getOperand(MemIdx + 4);

  // In the cases below, we want to promote any registers in the
  // memory operand to 64 bits.
  if (isAbsoluteReg(Base.getReg()) && Index.getReg() == 0) {
    Base.setReg(getReg64(Base.getReg()));
    return;
  }

  if (Base.getReg() == 0 && isAbsoluteReg(Index.getReg()) &&
             Scale.getImm() == 1) {
    Base.setReg(getReg64(Index.getReg()));
    Index.setReg(0);
    return;
  }

  if (Index.getReg() == 0 && Base.getReg() == 0) {
    Base.setReg(X86::R15);
    return;
  }

  MCRegister ScratchReg32 = 0;
  if (FlagUseX86CallConvention) {
    ScratchReg32 = X86::R11D;
  } else if (ScratchReg != 0) {
    ScratchReg32 = getReg32(ScratchReg);
  } else {
    Error(Inst,
        "Not enough scratch registers when emitting sandboxed memory operation."
    );
  }

  if (isAbsoluteReg(Base.getReg()) && !isAbsoluteReg(Index.getReg())) {
    // Validation requires that for memory access, a 64-bit register is used,
    // with its upper 32 bits clobbered by a 32-bit move just before.
    // Move Index to 32 bit ScratchReg, and use ScratchReg32 instead of Index
    // memory access.
    MCInst MovIdxToScratchReg;
    MovIdxToScratchReg.setOpcode(X86::MOV32rr);
    MovIdxToScratchReg.addOperand(MCOperand::createReg(ScratchReg32));
    MovIdxToScratchReg.addOperand(
        MCOperand::createReg(getReg32(Index.getReg())));
    Out.emitInstruction(MovIdxToScratchReg, STI);

    Base.setReg(getReg64(Base.getReg()));
    Index.setReg(getReg64(ScratchReg32));
    return;
  }

  if (Index.getReg() == 0 && Base.getReg() != 0) {
    // Validation requires that for memory access, a 64-bit register is used,
    // with its upper 32 bits clobbered by a 32-bit move just before.
    // Move Base to 32 bit ScratchReg, and use ScratchReg32 instead of Base for
    // memory access.
    MCInst MovBaseToScratchReg;
    MovBaseToScratchReg.setOpcode(X86::MOV32rr);
    MovBaseToScratchReg.addOperand(MCOperand::createReg(ScratchReg32));
    MovBaseToScratchReg.addOperand(
        MCOperand::createReg(getReg32(Base.getReg())));
    Out.emitInstruction(MovBaseToScratchReg, STI);

    Index.setReg(getReg64(ScratchReg32));
    Base.setReg(X86::R15);
    return;
  }

  MCRegister ScratchReg64 = getReg64(ScratchReg32);
  MCRegister BaseReg64 = getReg64(Base.getReg());
  MCRegister IndexReg64 = getReg64(Index.getReg());

  MCInst Lea;
  Lea.setOpcode(X86::LEA64_32r);
  Lea.addOperand(MCOperand::createReg(ScratchReg32));
  Lea.addOperand(MCOperand::createReg(BaseReg64));
  Lea.addOperand(Scale);
  Lea.addOperand(MCOperand::createReg(IndexReg64));
  Lea.addOperand(Offset);
  Lea.addOperand(Segment);

  // Specical case if there is no base or scale
  if (Base.getReg() == 0 && Scale.getImm() == 1) {
    Lea.getOperand(1).setReg(IndexReg64); // Base
    Lea.getOperand(3).setReg(0);          // Index
  }

  Out.emitInstruction(Lea, STI);

  Base.setReg(X86::R15);
  Scale.setImm(1);
  Index.setReg(ScratchReg64);
  if (Offset.isImm()) {
    Offset.setImm(0);
  } else {
    Inst.erase(Inst.begin() + MemIdx + 3);
    Inst.insert(Inst.begin() + MemIdx + 3, MCOperand::createImm(0));
  }
}

// Returns true if sandboxing the memory operand specified at Idx of
// Inst will emit any auxillary instructions.
// Used in emitSandboxMemOps as a helper.
static bool willEmitSandboxInsts(const MCInst &Inst, int Idx) {
  const MCOperand &Base = Inst.getOperand(Idx);
  const MCOperand &Scale = Inst.getOperand(Idx + 1);
  const MCOperand &Index = Inst.getOperand(Idx + 2);

  if (isAbsoluteReg(Base.getReg()) && Index.getReg() == 0) {
    return false;
  } else if (Base.getReg() == 0 && isAbsoluteReg(Index.getReg()) &&
             Scale.getImm() == 1) {
    return false;
  }

  return true;
}

// Emits the instructions that are used to sandbox the memory operands.
// Modifies memory operands of Inst in place, but does NOT EMIT Inst.
// If any instructions are emitted, it will precede them with a .bundle_lock
// Returns true if any instructions were emitted, otherwise false.
// Note that this method can modify Inst, but still return false if no
// auxiliary sandboxing instructions were emitted.
// If EmitInstructions is set to false, this will not emit anything and return
// whether it would emit any auxiliary instructions.
bool X86::X86MCNaClExpander::emitSandboxMemOps(MCInst &Inst,
                                               MCRegister ScratchReg,
                                               MCStreamer &Out,
                                               const MCSubtargetInfo &STI,
                                               bool EmitInstructions) {

  const MCOperandInfo *OpInfo = InstInfo->get(Inst.getOpcode()).OpInfo;

  bool anyInstsEmitted = false;

  for (int i = 0, e = Inst.getNumOperands(); i < e; ++i) {
    if (OpInfo[i].OperandType == MCOI::OPERAND_MEMORY) {
      if (!anyInstsEmitted && willEmitSandboxInsts(Inst, i)) {
        if (!EmitInstructions)
          return true;

        Out.emitBundleLock(false);
        anyInstsEmitted = true;
      }
      emitSandboxMemOp(Inst, i, ScratchReg, Out, STI);
      i += 4;
    }
  }

  return anyInstsEmitted;
}

// Expands any operations that load to or store from memory, but do
// not explicitly modify the stack or base pointer.
void X86::X86MCNaClExpander::expandLoadStore(const MCInst &Inst,
                                             MCStreamer &Out,
                                             const MCSubtargetInfo &STI,
                                             bool EmitPrefixes) {
  assert(!explicitlyModifiesRegister(Inst, X86::RBP));
  assert(!explicitlyModifiesRegister(Inst, X86::RSP));

  // Optimize if we are doing a mov into a register
  bool ElideScratchReg = false;
  switch (Inst.getOpcode()) {
  case X86::MOV64rm:
  case X86::MOV32rm:
  case X86::MOV16rm:
  case X86::MOV8rm:
    ElideScratchReg = true;
  default:
    break;
  }

  MCInst SandboxedInst(Inst);

  MCRegister ScratchReg;
  if (ElideScratchReg)
    ScratchReg = Inst.getOperand(0).getReg();
  else if (numScratchRegs() > 0)
    ScratchReg = getScratchReg(0);
  else
    ScratchReg = 0;

  // Determine if the instruction requires sandboxing
  bool InstNeedsSandboxing = emitSandboxMemOps(SandboxedInst,
                                               ScratchReg,
                                               Out,
                                               STI,
                                               /*EmitInstructions=*/false);

  // We may want to load/store to a high byte register, which is not possible
  // in combination with using R15 for sandboxing.
  // Instead, rotate the target register so that the load/store operates on the
  // low byte instead.
  MCRegister RotateRegister = X86::NoRegister;
  if (InstNeedsSandboxing &&
      (SandboxedInst.getOpcode() == X86::MOV8rm_NOREX ||
       SandboxedInst.getOpcode() == X86::MOV8rm) &&
      isHighReg(SandboxedInst.getOperand(0).getReg())) {
    RotateRegister = SandboxedInst.getOperand(0).getReg();
    SandboxedInst.setOpcode(X86::MOV8rm);
    SandboxedInst.getOperand(0).setReg(
        getX86SubSuperRegisterOrZero(RotateRegister, 8, /*High=*/false));
  } else if (InstNeedsSandboxing &&
             (SandboxedInst.getOpcode() == X86::MOV8mr_NOREX ||
              SandboxedInst.getOpcode() == X86::MOV8mr) &&
             isHighReg(SandboxedInst.getOperand(5).getReg())) {
    RotateRegister = SandboxedInst.getOperand(5).getReg();
    SandboxedInst.setOpcode(X86::MOV8mr);
    SandboxedInst.getOperand(5).setReg(
        getX86SubSuperRegisterOrZero(RotateRegister, 8, /*High=*/false));
  }

  if (RotateRegister != X86::NoRegister) {
    MCInst RotateHtoLInst;
    RotateHtoLInst.setOpcode(X86::ROR64ri);
    RotateHtoLInst.addOperand(MCOperand::createReg(getReg64(RotateRegister)));
    RotateHtoLInst.addOperand(MCOperand::createReg(getReg64(RotateRegister)));
    RotateHtoLInst.addOperand(MCOperand::createImm(8));
    Out.emitInstruction(RotateHtoLInst, STI);
  }

  bool BundleLock = emitSandboxMemOps(SandboxedInst,
                                      ScratchReg,
                                      Out,
                                      STI,
                                      /*EmitInstructions=*/true);
  emitInstruction(SandboxedInst, Out, STI, EmitPrefixes);
  if (BundleLock)
    Out.emitBundleUnlock();

  if (RotateRegister != X86::NoRegister) {
    // Rotate the register back.
    MCInst RotateLtoHInst;
    RotateLtoHInst.setOpcode(X86::ROL64ri);
    RotateLtoHInst.addOperand(MCOperand::createReg(getReg64(RotateRegister)));
    RotateLtoHInst.addOperand(MCOperand::createReg(getReg64(RotateRegister)));
    RotateLtoHInst.addOperand(MCOperand::createImm(8));
    Out.emitInstruction(RotateLtoHInst, STI);
  }
}

static bool isStringOperation(const MCInst &Inst) {
  switch (Inst.getOpcode()) {
  case X86::CMPSB:
  case X86::CMPSW:
  case X86::CMPSL:
  case X86::CMPSQ:
  case X86::MOVSB:
  case X86::MOVSW:
  case X86::MOVSL:
  case X86::MOVSQ:
  case X86::STOSB:
  case X86::STOSW:
  case X86::STOSL:
  case X86::STOSQ:
    return true;
  }
  return false;
}

static void fixupStringOpReg(const MCOperand &Op, MCStreamer &Out,
                             const MCSubtargetInfo &STI) {
  clearHighBits(Op, Out, STI);

  MCInst Lea;
  Lea.setOpcode(X86::LEA64r);
  Lea.addOperand(MCOperand::createReg(getReg64(Op.getReg())));
  Lea.addOperand(MCOperand::createReg(X86::R15));
  Lea.addOperand(MCOperand::createImm(1));
  Lea.addOperand(MCOperand::createReg(getReg64(Op.getReg())));
  Lea.addOperand(MCOperand::createImm(0));
  Lea.addOperand(MCOperand::createReg(0));
  Out.emitInstruction(Lea, STI);
}

void X86::X86MCNaClExpander::expandStringOperation(
    const MCInst &Inst,
    MCStreamer &Out,
    const MCSubtargetInfo &STI,
		bool EmitPrefixes) {
  Out.emitBundleLock(false);
  switch (Inst.getOpcode()) {
  case X86::CMPSB:
  case X86::CMPSW:
  case X86::CMPSL:
  case X86::CMPSQ:
  case X86::MOVSB:
  case X86::MOVSW:
  case X86::MOVSL:
  case X86::MOVSQ:
    fixupStringOpReg(Inst.getOperand(0), Out, STI);
    fixupStringOpReg(Inst.getOperand(1), Out, STI);
    break;
  case X86::STOSB:
  case X86::STOSW:
  case X86::STOSL:
  case X86::STOSQ:
    fixupStringOpReg(Inst.getOperand(0), Out, STI);
    break;
  }
  emitInstruction(Inst, Out, STI, EmitPrefixes);
  Out.emitBundleUnlock();
}

static void demoteInst(MCInst &Inst, const MCInstrInfo &InstInfo) {
  unsigned NewOpc = demoteOpcode(Inst.getOpcode());
  Inst.setOpcode(NewOpc);
  // demote all general purpose 64 bit registers to 32 bits
  const MCOperandInfo *OpInfo = InstInfo.get(Inst.getOpcode()).OpInfo;
  for (int i = 0, e = Inst.getNumOperands(); i < e; ++i) {
    if (OpInfo[i].OperandType == MCOI::OPERAND_REGISTER) {
      assert(Inst.getOperand(i).isReg());
      MCRegister Reg = Inst.getOperand(i).getReg();
      if (getReg64(Reg) == Reg) {
        Inst.getOperand(i).setReg(getReg32(Reg));
      }
    }
  }
}

static void emitStackFixup(MCRegister StackReg, MCStreamer &Out,
                           const MCSubtargetInfo &STI) {
  MCInst Lea;
  Lea.setOpcode(X86::LEA64r);
  Lea.addOperand(MCOperand::createReg(StackReg));
  Lea.addOperand(MCOperand::createReg(StackReg));
  Lea.addOperand(MCOperand::createImm(1));
  Lea.addOperand(MCOperand::createReg(X86::R15));
  Lea.addOperand(MCOperand::createImm(0));
  Lea.addOperand(MCOperand::createReg(0));
  Out.emitInstruction(Lea, STI);
}

void X86::X86MCNaClExpander::expandExplicitStackManipulation(
    MCRegister StackReg, const MCInst &Inst, MCStreamer &Out,
    const MCSubtargetInfo &STI, bool EmitPrefixes) {
  // First, handle special cases where sandboxing is not required.
  if (Inst.getOpcode() == X86::MOV64rr) {
    MCRegister SrcReg = Inst.getOperand(1).getReg();
    if (SrcReg == X86::RSP || SrcReg == X86::RBP)
      return emitInstruction(Inst, Out, STI, EmitPrefixes);
  }
  if (Inst.getOpcode() == X86::AND64ri8) {
    int Imm = Inst.getOperand(2).getImm();
    if (-128 <= Imm && Imm <= -1 && StackReg == X86::RSP)
      return emitInstruction(Inst, Out, STI, EmitPrefixes);
  }
  // Handle cases that don't directly access memory.
  if (is32BitStackPtrToFramePtrMove(Inst)) {
    // Transform movl %esp %ebp to movq %rsp %rbp to preserve upper 32 bits and
    // to permit validation.
    MCInst SandboxedInst(Inst);
    SandboxedInst.setOpcode(X86::MOV64rr);
    SandboxedInst.getOperand(0).setReg(getReg64(Inst.getOperand(0).getReg()));
    SandboxedInst.getOperand(1).setReg(getReg64(Inst.getOperand(1).getReg()));
    return emitInstruction(SandboxedInst, Out, STI, EmitPrefixes);
  }
  if (Inst.getOpcode() == X86::POP64r) {
    // Transform
    // pop   %rbp
    // into
    // pop %r11
    // .bundle_lock
    // movl %r11d, %ebp
    // add %r15, %rbp
    // .bundle_unlock
    // where %r11 is the scratch register, and %rbp the stack register.

    MCInst PopR11Inst;
    PopR11Inst.setOpcode(X86::POP64r);
    PopR11Inst.addOperand(MCOperand::createReg(X86::R11));
    Out.emitInstruction(PopR11Inst, STI);

    Out.emitBundleLock(false);

    MCInst MovR11ToEBP;
    MovR11ToEBP.setOpcode(X86::MOV32rr);
    MovR11ToEBP.addOperand(MCOperand::createReg(getReg32(StackReg)));
    MovR11ToEBP.addOperand(MCOperand::createReg(X86::R11D));
    Out.emitInstruction(MovR11ToEBP, STI);

    MCInst AddR15Inst;
    AddR15Inst.setOpcode(X86::ADD64rr);
    AddR15Inst.addOperand(MCOperand::createReg(StackReg));
    AddR15Inst.addOperand(MCOperand::createReg(StackReg));
    AddR15Inst.addOperand(MCOperand::createReg(X86::R15));
    Out.emitInstruction(AddR15Inst, STI);

    return Out.emitBundleUnlock();
  }

  MCInst SandboxedInst(Inst);
  demoteInst(SandboxedInst, *InstInfo);

  MCRegister ScratchReg;
  if (numScratchRegs() > 0)
    ScratchReg = getScratchReg(0);
  else
    ScratchReg = 0;

  bool MemSandboxed = emitSandboxMemOps(SandboxedInst,
                                        ScratchReg,
                                        Out,
                                        STI,
                                        /*emitInstructions=*/true);

  Out.emitBundleLock(false); // for stack fixup

  emitInstruction(SandboxedInst, Out, STI, EmitPrefixes);
  if (MemSandboxed)
    Out.emitBundleUnlock(); // for memory reference
  emitStackFixup(StackReg, Out, STI);

  Out.emitBundleUnlock(); // for stack fixup
}

void X86::X86MCNaClExpander::expandStackRegPush(const MCInst &Inst,
                                                MCStreamer &Out,
                                                const MCSubtargetInfo &STI) {
  // Transform
  // push   %rbp
  // into
  // movl   %ebp, %r11d
  // push   %r11
  // where %r11 is the scratch register, and %rbp the stack register.
  const MCRegister StackReg = Inst.getOperand(0).getReg();
  MCInst MovInst;
  MovInst.setOpcode(X86::MOV32rr);
  MovInst.addOperand(MCOperand::createReg(X86::R11D));
  MovInst.addOperand(MCOperand::createReg(getReg32(StackReg)));
  Out.emitInstruction(MovInst, STI);
  MCInst PushInst(Inst);
  PushInst.getOperand(0).setReg(X86::R11);
  return Out.emitInstruction(PushInst, STI);
}

// Returns true if Inst is an X86 prefix
static bool isPrefix(const MCInst &Inst) {
  switch (Inst.getOpcode()) {
  case X86::LOCK_PREFIX:
  case X86::REP_PREFIX:
  case X86::REPNE_PREFIX:
  case X86::REX64_PREFIX:
    return true;
  default:
    return false;
  }
}

// returns the stack register that is used in xchg instruction
static MCRegister XCHGStackReg(const MCInst &Inst) {
  MCRegister Reg1 = 0, Reg2 = 0;
  switch (Inst.getOpcode()) {
  case X86::XCHG64ar:
  case X86::XCHG64rm:
    Reg1 = Inst.getOperand(0).getReg();
    break;
  case X86::XCHG64rr:
    Reg1 = Inst.getOperand(0).getReg();
    Reg2 = Inst.getOperand(2).getReg();
    break;
  default:
    return 0;
  }
  if (Reg1 == X86::RSP || Reg1 == X86::RBP)
    return Reg1;

  if (Reg2 == X86::RSP || Reg2 == X86::RBP)
    return Reg2;

  return 0;
}

// Emit prefixes + instruction if EmitPrefixes argument is true.
// Otherwise, emit the bare instruction.
void X86::X86MCNaClExpander::emitInstruction(const MCInst &Inst,
                                             MCStreamer &Out,
                                             const MCSubtargetInfo &STI,
                                             bool EmitPrefixes) {
  if (EmitPrefixes) {
    for (const MCInst &Prefix : Prefixes)
      Out.emitInstruction(Prefix, STI);
    Prefixes.clear();
  }
  Out.emitInstruction(Inst, STI);
}


void X86::X86MCNaClExpander::doExpandInst(const MCInst &Inst,
                                          MCStreamer &Out,
                                          const MCSubtargetInfo &STI,
                                          bool EmitPrefixes) {
  if (isPrefix(Inst)) {
    return Prefixes.push_back(Inst);
  }
  if (isDirectCall(Inst)) {
    expandDirectCall(Inst, Out, STI);
  } else if (isIndirectBranch(Inst) || isCall(Inst)) {
    expandIndirectBranch(Inst, Out, STI);
  } else if (isReturn(Inst)) {
    expandReturn(Inst, Out, STI);
  } else if (Is64Bit && isStringOperation(Inst)) {
    expandStringOperation(Inst, Out, STI, EmitPrefixes);
  } else if (Is64Bit && explicitlyModifiesRegister(Inst, X86::RSP)) {
    expandExplicitStackManipulation(X86::RSP, Inst, Out, STI, EmitPrefixes);
  } else if (Is64Bit && explicitlyModifiesRegister(Inst, X86::RBP)) {
    expandExplicitStackManipulation(X86::RBP, Inst, Out, STI, EmitPrefixes);
  } else if (MCRegister StackReg = XCHGStackReg(Inst)) {
    // the above case doesnt catch xchg instruction, so special case
    expandExplicitStackManipulation(StackReg, Inst, Out, STI, EmitPrefixes);
  } else if (Is64Bit && isStackRegPush(Inst)) {
    expandStackRegPush(Inst, Out, STI);
  } else if (Is64Bit) {
    return expandLoadStore(Inst, Out, STI, EmitPrefixes);
  } else {
    emitInstruction(Inst, Out, STI, EmitPrefixes);
  }
}

bool X86::X86MCNaClExpander::expandInst(const MCInst &Inst, MCStreamer &Out,
                                        const MCSubtargetInfo &STI) {
  if (Guard)
    return false;
  Guard = true;

  doExpandInst(Inst, Out, STI, true);
  invalidateScratchRegs(Inst);

  Guard = false;
  return true;
}

static unsigned demoteOpcode(unsigned Opcode) {
  switch (Opcode) {
  case X86::ADC64rr:
    return X86::ADC32rr;
  case X86::ADC64ri8:
    return X86::ADC32ri8;
  case X86::ADC64ri32:
    return X86::ADC32ri;
  case X86::ADC64rm:
    return X86::ADC32rm;
  case X86::ADCX64rr:
    return X86::ADCX32rr;
  case X86::ADCX64rm:
    return X86::ADCX32rm;
  case X86::ADD64rr:
    return X86::ADD32rr;
  case X86::ADD64ri8:
    return X86::ADD32ri8;
  case X86::ADD64ri32:
    return X86::ADD32ri;
  case X86::ADD64rm:
    return X86::ADD32rm;
  case X86::ADOX64rr:
    return X86::ADOX32rr;
  case X86::ADOX64rm:
    return X86::ADOX32rm;
  case X86::ANDN64rr:
    return X86::ANDN32rr;
  case X86::ANDN64rm:
    return X86::ANDN32rm;
  case X86::AND64rr:
    return X86::AND32rr;
  case X86::AND64ri8:
    return X86::AND32ri8;
  case X86::AND64ri32:
    return X86::AND32ri;
  case X86::AND64rm:
    return X86::AND32rm;
  case X86::BEXTRI64ri:
    return X86::BEXTRI32ri;
  case X86::BEXTRI64mi:
    return X86::BEXTRI32mi;
  case X86::BEXTR64rr:
    return X86::BEXTR32rr;
  case X86::BEXTR64rm:
    return X86::BEXTR32rm;
  case X86::BLCFILL64rr:
    return X86::BLCFILL32rr;
  case X86::BLCFILL64rm:
    return X86::BLCFILL32rm;
  case X86::BLCI64rr:
    return X86::BLCI32rr;
  case X86::BLCI64rm:
    return X86::BLCI32rm;
  case X86::BLCIC64rr:
    return X86::BLCIC32rr;
  case X86::BLCIC64rm:
    return X86::BLCIC32rm;
  case X86::BLCMSK64rr:
    return X86::BLCMSK32rr;
  case X86::BLCMSK64rm:
    return X86::BLCMSK32rm;
  case X86::BLCS64rr:
    return X86::BLCS32rr;
  case X86::BLCS64rm:
    return X86::BLCS32rm;
  case X86::BLSFILL64rr:
    return X86::BLSFILL32rr;
  case X86::BLSFILL64rm:
    return X86::BLSFILL32rm;
  case X86::BLSIC64rr:
    return X86::BLSIC32rr;
  case X86::BLSIC64rm:
    return X86::BLSIC32rm;
  case X86::BLSI64rr:
    return X86::BLSI32rr;
  case X86::BLSI64rm:
    return X86::BLSI32rm;
  case X86::BLSMSK64rr:
    return X86::BLSMSK32rr;
  case X86::BLSMSK64rm:
    return X86::BLSMSK32rm;
  case X86::BLSR64rr:
    return X86::BLSR32rr;
  case X86::BLSR64rm:
    return X86::BLSR32rm;
  case X86::BSF64rr:
    return X86::BSF32rr;
  case X86::BSF64rm:
    return X86::BSF32rm;
  case X86::BSR64rr:
    return X86::BSR32rr;
  case X86::BSR64rm:
    return X86::BSR32rm;
  case X86::BSWAP64r:
    return X86::BSWAP32r;
  case X86::BTC64rr:
    return X86::BTC32rr;
  case X86::BTC64ri8:
    return X86::BTC32ri8;
  case X86::BT64rr:
    return X86::BT32rr;
  case X86::BT64ri8:
    return X86::BT32ri8;
  case X86::BTR64rr:
    return X86::BTR32rr;
  case X86::BTR64ri8:
    return X86::BTR32ri8;
  case X86::BTS64rr:
    return X86::BTS32rr;
  case X86::BTS64ri8:
    return X86::BTS32ri8;
  case X86::BZHI64rr:
    return X86::BZHI32rr;
  case X86::BZHI64rm:
    return X86::BZHI32rm;
  case X86::CALL64r:
    return X86::CALL32r;
  case X86::XOR64rr:
    return X86::XOR32rr;
  // NaCl 2020 note: there used to be a lot of cmov opcodes, now there are only
  // two left since they were unified. The condition (above, equal, ...) used
  // to be part of the opcode, now it is encoded  differently as per
  // https://github.com/llvm/llvm-project/commit/e0bfeb5f24979416144c16e8b99204f5f163b889
  // If there are errors, maybe the reason could be that here it is not sufficient
  // to just lower the opcode, but the operation needs to be constructed
  // in a more sophisticated way.
  case X86::CMOV64rr:
    return X86::CMOV32rr;
  case X86::CMOV64rm:
    return X86::CMOV32rm;
  case X86::CMP64rr:
    return X86::CMP32rr;
  case X86::CMP64ri8:
    return X86::CMP32ri8;
  case X86::CMP64ri32:
    return X86::CMP32ri;
  case X86::CMP64rm:
    return X86::CMP32rm;
  case X86::CMPXCHG64rr:
    return X86::CMPXCHG32rr;
  case X86::CRC32r64r8:
    return X86::CRC32r32r8;
  case X86::CRC32r64m8:
    return X86::CRC32r64m8;
  case X86::CRC32r64r64:
    return X86::CRC32r32r32;
  case X86::CRC32r64m64:
    return X86::CRC32r32m32;
  case X86::CVTSD2SI64rr_Int:
    return X86::CVTSD2SIrr_Int;
  case X86::CVTSD2SI64rm_Int:
    return X86::CVTSD2SIrm_Int;
  case X86::CVTSS2SI64rr_Int:
    return X86::CVTSS2SIrr_Int;
  case X86::CVTSS2SI64rm_Int:
    return X86::CVTSS2SIrm_Int;
  case X86::CVTTSD2SI64rr:
    return X86::CVTTSD2SIrr;
  case X86::CVTTSD2SI64rm:
    return X86::CVTTSD2SIrm;
  case X86::CVTTSS2SI64rr:
    return X86::CVTTSS2SIrr;
  case X86::CVTTSS2SI64rm:
    return X86::CVTTSS2SIrm;
  case X86::DEC64r:
    return X86::DEC32r;
  case X86::DIV64r:
    return X86::DIV32r;
  case X86::IDIV64r:
    return X86::IDIV32r;
  case X86::IMUL64r:
    return X86::IMUL32r;
  case X86::IMUL64rr:
    return X86::IMUL32rr;
  case X86::IMUL64rri8:
    return X86::IMUL32rri8;
  case X86::IMUL64rri32:
    return X86::IMUL32rri;
  case X86::IMUL64rm:
    return X86::IMUL32rm;
  case X86::IMUL64rmi8:
    return X86::IMUL32rmi8;
  case X86::IMUL64rmi32:
    return X86::IMUL32rmi;
  case X86::INC64r:
    return X86::INC32r;
  case X86::INVEPT64:
    return X86::INVEPT32;
  case X86::INVPCID64:
    return X86::INVPCID32;
  case X86::INVVPID64:
    return X86::INVVPID32;
  case X86::JMP64r:
    return X86::JMP32r;
  case X86::KMOVQrk:
    return X86::KMOVQrk;
  case X86::LAR64rr:
    return X86::LAR32rr;
  case X86::LAR64rm:
    return X86::LAR32rm;
  case X86::LEA64r:
    return X86::LEA32r;
  case X86::LFS64rm:
    return X86::LFS32rm;
  case X86::LGS64rm:
    return X86::LGS32rm;
  case X86::LSL64rr:
    return X86::LSL32rr;
  case X86::LSL64rm:
    return X86::LSL32rm;
  case X86::LSS64rm:
    return X86::LSS32rm;
  case X86::LZCNT64rr:
    return X86::LZCNT32rr;
  case X86::LZCNT64rm:
    return X86::LZCNT32rm;
  case X86::MOV64ri:
    return X86::MOV32ri;
  case X86::MOVBE64rm:
    return X86::MOVBE32rm;
  case X86::MOV64rr:
    return X86::MOV32rr;
  case X86::MMX_MOVD64from64rr:
    return X86::MMX_MOVD64grr;
  case X86::MOVPQIto64rr:
    return X86::MOVPDI2DIrr;
  case X86::MOV64rs:
    return X86::MOV32rs;
  case X86::MOV64rd:
    return X86::MOV32rd;
  case X86::MOV64rc:
    return X86::MOV32rc;
  case X86::MOV64ri32:
    return X86::MOV32ri;
  case X86::MOV64rm:
    return X86::MOV32rm;
  case X86::MOVSX64rr8:
    return X86::MOVSX32rr8;
  case X86::MOVSX64rm8:
    return X86::MOVSX32rm8;
  case X86::MOVSX64rr32:
    return X86::MOV32rr;
  case X86::MOVSX64rm32:
    return X86::MOV32rm;
  case X86::MOVSX64rr16:
    return X86::MOVSX32rr16;
  case X86::MOVSX64rm16:
    return X86::MOVSX32rm16;
  case X86::MOVZX64rr8:
    return X86::MOVZX32rr8;
  case X86::MOVZX64rm8:
    return X86::MOVZX32rm8;
  case X86::MOVZX64rr16:
    return X86::MOVZX32rr16;
  case X86::MOVZX64rm16:
    return X86::MOVZX32rm16;
  case X86::MUL64r:
    return X86::MUL32r;
  case X86::MULX64rr:
    return X86::MULX32rr;
  case X86::MULX64rm:
    return X86::MULX32rm;
  case X86::NEG64r:
    return X86::NEG32r;
  case X86::NOT64r:
    return X86::NOT32r;
  case X86::OR64rr:
    return X86::OR32rr;
  case X86::OR64ri8:
    return X86::OR32ri8;
  case X86::OR64ri32:
    return X86::OR32ri;
  case X86::OR64rm:
    return X86::OR32rm;
  case X86::PDEP64rr:
    return X86::PDEP32rr;
  case X86::PDEP64rm:
    return X86::PDEP32rm;
  case X86::PEXT64rr:
    return X86::PEXT32rr;
  case X86::PEXT64rm:
    return X86::PEXT32rm;
  case X86::PEXTRQrr:
    return X86::PEXTRQrr;
  case X86::POPCNT64rr:
    return X86::POPCNT32rr;
  case X86::POPCNT64rm:
    return X86::POPCNT32rm;
  case X86::POP64r:
    return X86::POP32r;
  case X86::POP64rmr:
    return X86::POP32rmr;
  // TODO POP64rmm?
  case X86::PUSH64r:
    return X86::PUSH32r;
  case X86::PUSH64rmr:
    return X86::PUSH32rmr;
  case X86::RCL64r1:
    return X86::RCL32r1;
  case X86::RCL64rCL:
    return X86::RCL32rCL;
  case X86::RCL64ri:
    return X86::RCL32ri;
  case X86::RCR64r1:
    return X86::RCR32r1;
  case X86::RCR64rCL:
    return X86::RCR32rCL;
  case X86::RCR64ri:
    return X86::RCR32ri;
  case X86::RDFSBASE64:
    return X86::RDFSBASE;
  case X86::RDGSBASE64:
    return X86::RDGSBASE;
  case X86::RDRAND64r:
    return X86::RDRAND32r;
  case X86::RDSEED64r:
    return X86::RDSEED32r;
  case X86::ROL64r1:
    return X86::ROL32r1;
  case X86::ROL64rCL:
    return X86::ROL32rCL;
  case X86::ROL64ri:
    return X86::ROL32ri;
  case X86::ROR64r1:
    return X86::ROR32r1;
  case X86::ROR64rCL:
    return X86::ROR32rCL;
  case X86::ROR64ri:
    return X86::ROR32ri;
  case X86::RORX64ri:
    return X86::RORX32ri;
  case X86::RORX64mi:
    return X86::RORX64mi;
  case X86::SAR64r1:
    return X86::SAR32r1;
  case X86::SAR64rCL:
    return X86::SAR32rCL;
  case X86::SAR64ri:
    return X86::SAR32ri;
  case X86::SARX64rr:
    return X86::SARX32rr;
  case X86::SARX64rm:
    return X86::SARX32rm;
  case X86::SBB64rr:
    return X86::SBB32rr;
  case X86::SBB64ri8:
    return X86::SBB32ri8;
  case X86::SBB64ri32:
    return X86::SBB32ri;
  case X86::SBB64rm:
    return X86::SBB32rm;
  case X86::SHLD64rrCL:
    return X86::SHLD32rrCL;
  case X86::SHLD64rri8:
    return X86::SHLD32rri8;
  case X86::SHL64r1:
    return X86::SHL32r1;
  case X86::SHL64rCL:
    return X86::SHL32rCL;
  case X86::SHL64ri:
    return X86::SHL32ri;
  case X86::SHLX64rr:
    return X86::SHLX32rr;
  case X86::SHLX64rm:
    return X86::SHLX32rm;
  case X86::SHRD64rrCL:
    return X86::SHRD32rrCL;
  case X86::SHRD64rri8:
    return X86::SHRD32rri8;
  case X86::SHR64r1:
    return X86::SHR32r1;
  case X86::SHR64rCL:
    return X86::SHR32rCL;
  case X86::SHR64ri:
    return X86::SHR32ri;
  case X86::SHRX64rr:
    return X86::SHRX32rr;
  case X86::SHRX64rm:
    return X86::SHRX32rm;
  case X86::SLDT64r:
    return X86::SLDT32r;
  case X86::SMSW64r:
    return X86::SMSW32r;
  case X86::STR64r:
    return X86::STR32r;
  case X86::SUB64rr:
    return X86::SUB32rr;
  case X86::SUB64ri8:
    return X86::SUB32ri8;
  case X86::SUB64ri32:
    return X86::SUB32ri;
  case X86::SUB64rm:
    return X86::SUB32rm;
  case X86::T1MSKC64rr:
    return X86::T1MSKC32rr;
  case X86::T1MSKC64rm:
    return X86::T1MSKC32rm;
  case X86::TEST64rr:
    return X86::TEST32rr;
  case X86::TEST64ri32:
    return X86::TEST32ri;
  case X86::TEST64mr:
    return X86::TEST32mr;
  case X86::TZCNT64rr:
    return X86::TZCNT32rr;
  case X86::TZCNT64rm:
    return X86::TZCNT32rm;
  case X86::TZMSK64rr:
    return X86::TZMSK32rr;
  case X86::TZMSK64rm:
    return X86::TZMSK32rm;
  /*
  Some more Candidates for this are:
  // Candidates start
  case X86::VCVTSI2SSZrr:
  case X86::VCVTSI2SSZrm:
  case X86::Int_VCVTSI2SSZrr:
  case X86::Int_VCVTSI2SSZrm:
  case X86::VCVTSI2SSZrr_Int:
  case X86::VCVTSI2SSZrm_Int:
  case X86::VCVTSI642SSZrr:
  case X86::VCVTSI642SSZrm:
  case X86::Int_VCVTSI2SS64Zrr:
  case X86::Int_VCVTSI2SS64Zrm:
  case X86::VCVTSI642SSZrr_Int:
  case X86::VCVTSI642SSZrm_Int:
  case X86::VCVTSI2SDZrr:
  case X86::VCVTSI2SDZrm:
  case X86::Int_VCVTSI2SDZrr:
  case X86::Int_VCVTSI2SDZrm:
  case X86::VCVTSI2SDZrr_Int:
  case X86::VCVTSI2SDZrm_Int:
  case X86::VCVTSI642SDZrr:
  case X86::VCVTSI642SDZrm:
  case X86::Int_VCVTSI2SD64Zrr:
  case X86::Int_VCVTSI2SD64Zrm:
  case X86::VCVTSI642SDZrr_Int:
  case X86::VCVTSI642SDZrm_Int:
  Maybe everything that is listed in hasUndefRegUpdate() in X86InstrInfo.cpp
  // Candidates end
  */
  case X86::VCVTSD2SI64rr_Int:
    return X86::VCVTSD2SIrr_Int;
  case X86::VCVTSD2SI64Zrr_Int:
    return X86::VCVTSD2SIZrr_Int;
  case X86::VCVTSD2SI64Zrm_Int:
    return X86::VCVTSD2SIZrm_Int;
  case X86::VCVTSD2SI64rm_Int:
    return X86::VCVTSD2SIrm_Int;
  case X86::VCVTSD2USI64Zrr_Int:
    return X86::VCVTSD2USIZrr_Int;
  case X86::VCVTSD2USI64Zrm_Int:
    return X86::VCVTSD2USIZrm_Int;
  case X86::VCVTSS2SI64rr_Int:
    return X86::VCVTSS2SIrr_Int;
  case X86::VCVTSS2SI64Zrr_Int:
    return X86::VCVTSS2SIZrr_Int;
  case X86::VCVTSS2SI64Zrm_Int:
    return X86::VCVTSS2SIZrm_Int;
  case X86::VCVTSS2SI64rm_Int:
    return X86::VCVTSS2SIrm_Int;
  case X86::VCVTSS2USI64Zrr_Int:
    return X86::VCVTSS2USIZrr_Int;
  case X86::VCVTSS2USI64Zrm_Int:
    return X86::VCVTSS2USIZrm_Int;
  case X86::VCVTTSD2SI64rr:
    return X86::VCVTTSD2SIrr;
  case X86::VCVTTSD2SI64Zrr:
    return X86::VCVTTSD2SIZrr;
  case X86::VCVTTSD2SI64Zrm:
    return X86::VCVTTSD2SIZrm;
  case X86::VCVTTSD2SI64rm:
    return X86::VCVTTSD2SIrm;
  case X86::VCVTTSD2USI64Zrr:
    return X86::VCVTTSD2USIZrr;
  case X86::VCVTTSD2USI64Zrm:
    return X86::VCVTTSD2USIZrm;
  case X86::VCVTTSS2SI64rr:
    return X86::VCVTTSS2SIrr;
  case X86::VCVTTSS2SI64Zrr:
    return X86::VCVTTSS2SIZrr;
  case X86::VCVTTSS2SI64Zrm:
    return X86::VCVTTSS2SIZrm;
  case X86::VCVTTSS2SI64rm:
    return X86::VCVTTSS2SIrm;
  case X86::VCVTTSS2USI64Zrr:
    return X86::VCVTTSS2USIZrr;
  case X86::VCVTTSS2USI64Zrm:
    return X86::VCVTTSS2USIZrm;
  case X86::VMOVPQIto64rr:
    return X86::VMOVPDI2DIrr;
  case X86::VMOVPQIto64Zrr:
    return X86::VMOVPDI2DIZrr;
  case X86::VMREAD64rr:
    return X86::VMREAD32rr;
  case X86::VMWRITE64rr:
    return X86::VMWRITE32rr;
  case X86::VMWRITE64rm:
    return X86::VMWRITE32rm;
  case X86::VPEXTRQrr:
    return X86::VPEXTRQrr;
  case X86::WRFSBASE64:
    return X86::WRFSBASE;
  case X86::WRGSBASE64:
    return X86::WRGSBASE;
  case X86::XADD64rr:
    return X86::XADD32rr;
  case X86::XCHG64ar:
    return X86::XCHG32ar;
  case X86::XCHG64rr:
    return X86::XCHG32rr;
  case X86::XCHG64rm:
    return X86::XCHG32rm;
  case X86::XOR64ri8:
    return X86::XOR32ri8;
  case X86::XOR64ri32:
    return X86::XOR32ri;
  case X86::XOR64rm:
    return X86::XOR32rm;
  default:
    return Opcode;
  }
}
