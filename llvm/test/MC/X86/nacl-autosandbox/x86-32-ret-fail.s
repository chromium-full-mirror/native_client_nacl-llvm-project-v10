// RUN: not llvm-mc -filetype asm -triple i386-unknown-nacl -nacl-use-x86-call-convention=false %s

// Tests that the assembler fails if not given a scratch register in
// nacl-use-x86-call-convention=false mode.

	ret
