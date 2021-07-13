//=== X86NaClRewritePAss.cpp - Rewrite instructions for NaCl SFI --*- C++ -*-=//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#define DEBUG_TYPE "nacl-rewrite-pass"

#include "X86.h"
#include "X86InstrInfo.h"
#include "X86Subtarget.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace {
  class X86NaClRewritePass : public MachineFunctionPass {
  public:
    static char ID;
    X86NaClRewritePass() : MachineFunctionPass(ID) {}

    virtual bool runOnMachineFunction(MachineFunction &Fn) override;

    virtual StringRef getPassName() const override {
      return "NaCl Rewrites";
    }

  private:
    const TargetInstrInfo *TII;
    const X86Subtarget *Subtarget;

    bool runOnMachineBasicBlock(MachineBasicBlock &MBB);

    bool ApplyRewrites(MachineBasicBlock &MBB,
                      MachineBasicBlock::iterator MBBI);
  };

  char X86NaClRewritePass::ID = 0;
}


bool X86NaClRewritePass::ApplyRewrites(MachineBasicBlock &MBB,
                                       MachineBasicBlock::iterator MBBI) {
  MachineInstr &MI = *MBBI;
  DebugLoc DL = MI.getDebugLoc();
  unsigned Opc = MI.getOpcode();

  // General Dynamic NaCl TLS model
  // http://code.google.com/p/nativeclient/issues/detail?id=1685
  if (Opc == X86::NACL_CG_GD_TLS_addr64) {

    // Rewrite to:
    //   leaq $sym@TLSGD(%rip), %rdi
    //   call __tls_get_addr@PLT
    BuildMI(MBB, MBBI, DL, TII->get(X86::LEA64r), X86::RDI)
        .addReg(X86::RIP) // Base
        .addImm(1) // Scale
        .addReg(0) // Index
        .addGlobalAddress(MI.getOperand(3).getGlobal(), 0,
                          MI.getOperand(3).getTargetFlags())
        .addReg(0); // Segment
    BuildMI(MBB, MBBI, DL, TII->get(X86::CALL64pcrel32))
        .addExternalSymbol("__tls_get_addr", X86II::MO_PLT);
    MI.eraseFromParent();
    return true;
  }

  // Local Exec NaCl TLS Model
  if (Opc == X86::NACL_CG_LE_TLS_addr64 ||
      Opc == X86::NACL_CG_LE_TLS_addr32) {
    unsigned CallOpc, LeaOpc, Reg;
    // Rewrite to:
    //   call __nacl_read_tp@PLT
    //   lea $sym@flag(,%reg), %reg
    if (Opc == X86::NACL_CG_LE_TLS_addr64) {
      CallOpc = X86::CALL64pcrel32;
      LeaOpc = X86::LEA64r;
      Reg = X86::RAX;
    } else {
      CallOpc = X86::CALLpcrel32;
      LeaOpc = X86::LEA32r;
      Reg = X86::EAX;
    }
    BuildMI(MBB, MBBI, DL, TII->get(CallOpc))
        .addExternalSymbol("__nacl_read_tp", X86II::MO_PLT);
    BuildMI(MBB, MBBI, DL, TII->get(LeaOpc), Reg)
        .addReg(0) // Base
        .addImm(1) // Scale
        .addReg(Reg) // Index
        .addGlobalAddress(MI.getOperand(3).getGlobal(), 0,
                          MI.getOperand(3).getTargetFlags())
        .addReg(0); // Segment
    MI.eraseFromParent();
    return true;
  }

  // Initial Exec NaCl TLS Model
  if (Opc == X86::NACL_CG_IE_TLS_addr64 ||
      Opc == X86::NACL_CG_IE_TLS_addr32) {
    unsigned CallOpc, AddOpc, Base, Reg;
    // Rewrite to:
    //   call __nacl_read_tp@PLT
    //   addq sym@flag(%base), %reg
    if (Opc == X86::NACL_CG_IE_TLS_addr64) {
      CallOpc = X86::CALL64pcrel32;
      AddOpc = X86::ADD64rm;
      Base = X86::RIP;
      Reg = X86::RAX;
    } else {
      CallOpc = X86::CALLpcrel32;
      AddOpc = X86::ADD32rm;
      Base = MI.getOperand(3).getTargetFlags() == X86II::MO_INDNTPOFF ?
          0 : X86::EBX; // EBX for GOTNTPOFF.
      Reg = X86::EAX;
    }
    BuildMI(MBB, MBBI, DL, TII->get(CallOpc))
        .addExternalSymbol("__nacl_read_tp", X86II::MO_PLT);
    BuildMI(MBB, MBBI, DL, TII->get(AddOpc), Reg)
        .addReg(Reg)
        .addReg(Base)
        .addImm(1) // Scale
        .addReg(0) // Index
        .addGlobalAddress(MI.getOperand(3).getGlobal(), 0,
                          MI.getOperand(3).getTargetFlags())
        .addReg(0); // Segment
    MI.eraseFromParent();
    return true;
  }

  return false;
}

bool X86NaClRewritePass::runOnMachineFunction(MachineFunction &MF) {
  bool Modified = false;

  TII = MF.getSubtarget().getInstrInfo();
  Subtarget = &MF.getSubtarget<X86Subtarget>();
  assert(Subtarget->isTargetNaCl() && "Unexpected target in NaClRewritePass!");

  LLVM_DEBUG(dbgs() << "*************** NaCl Rewrite Pass ***************\n");
  for (MachineFunction::iterator MFI = MF.begin(), E = MF.end();
       MFI != E;
       ++MFI) {
    Modified |= runOnMachineBasicBlock(*MFI);
  }
  LLVM_DEBUG(dbgs() << "*************** NaCl Rewrite DONE  ***************\n");
  return Modified;
}

bool X86NaClRewritePass::runOnMachineBasicBlock(MachineBasicBlock &MBB) {
  bool Modified = false;
  if (MBB.hasAddressTaken()) {
    MBB.setAlignment(llvm::Align(32));
    Modified = true;
  }
  for (MachineBasicBlock::iterator MBBI = MBB.begin(), NextMBBI = MBBI;
       MBBI != MBB.end(); MBBI = NextMBBI) {
    ++NextMBBI;
    // When one of these methods makes a change,
    // it returns true, skipping the others.
    if (ApplyRewrites(MBB, MBBI)) {
      Modified = true;
    }
  }
  return Modified;
}

/// createX86NaClRewritePassPass - returns an instance of the pass.
namespace llvm {
  FunctionPass* createX86NaClRewritePass() {
    return new X86NaClRewritePass();
  }
}
