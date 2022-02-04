// RUN: llvm-mc -filetype asm -triple i386-unknown-nacl %s
// XFAIL: *

// Tests that the assembler fails if not given a scratch register

	jmp *12(%ecx)
