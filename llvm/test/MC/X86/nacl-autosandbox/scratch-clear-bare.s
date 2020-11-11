// RUN: llvm-mc -filetype asm -triple i386-unknown-nacl %s

// Tests that a bare .scratch_clear does not fail.

.scratch_clear
