// RUN: llvm-mc -filetype asm -triple i386-unknown-nacl %s

// Tests a basic .scratch and .scratch_clear sequence, should succeed

.scratch %ecx
.scratch_clear
