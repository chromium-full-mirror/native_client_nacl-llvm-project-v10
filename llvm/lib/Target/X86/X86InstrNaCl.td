//====- X86InstrNaCl.td - Describe NaCl Instructions ----*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file describes the modifications to the X86 instruction set needed for
// Native Client code generation.
//
//===----------------------------------------------------------------------===//


// ELF TLS Support

// These are lowered in X86NaClRewritePass.
let Defs = [RAX, RCX, RDX, RSI, RDI, R8, R9, R10, R11,
            FP0, FP1, FP2, FP3, FP4, FP5, FP6, ST0, ST1,
            MM0, MM1, MM2, MM3, MM4, MM5, MM6, MM7,
            XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7,
            XMM8, XMM9, XMM10, XMM11, XMM12, XMM13, XMM14, XMM15, EFLAGS],
    Uses = [RSP] in {
def NACL_CG_GD_TLS_addr64 : I<0, Pseudo, (outs), (ins i32mem:$sym), "",
                            [(X86tlsaddr tls32addr:$sym)]>,
                            Requires<[IsNaCl, In64BitMode]>;
def NACL_CG_LE_TLS_addr64 : I<0, Pseudo, (outs), (ins i32mem:$sym), "",
                            [(X86tlsaddr_le tls32addr:$sym)]>,
                            Requires<[IsNaCl, In64BitMode]>;
def NACL_CG_IE_TLS_addr64 : I<0, Pseudo, (outs), (ins i32mem:$sym), "",
                            [(X86tlsaddr_ie tls32addr:$sym)]>,
                            Requires<[IsNaCl, In64BitMode]>;
// For mtls-use-call.
def NACL_CG_LE_TLS_addr32 : I<0, Pseudo, (outs), (ins i32mem:$sym), "",
                            [(X86tlsaddr_le tls32addr:$sym)]>,
                            Requires<[IsNaCl, Not64BitMode]>;
def NACL_CG_IE_TLS_addr32 : I<0, Pseudo, (outs), (ins i32mem:$sym), "",
                            [(X86tlsaddr_ie tls32addr:$sym)]>,
                            Requires<[IsNaCl, Not64BitMode]>;
}
