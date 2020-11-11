// RUN: not llvm-mc -filetype asm -triple x86_64-unknown-nacl %s 2>&1 | FileCheck %s

.scratch %rbp
// CHECK: Register can't be used as a scratch register
