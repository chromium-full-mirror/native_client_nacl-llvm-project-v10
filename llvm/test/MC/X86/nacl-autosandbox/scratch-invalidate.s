// RUN: not llvm-mc -filetype asm -triple x86_64-unknown-nacl -nacl-use-x86-call-convention=false %s 2>&1 | FileCheck %s

.scratch %ecx
        mov (%rax, %rbx), %rax
        mov %eax, %ecx
        ret
// Checking both stdout and stderr is unreliable because of the ordering, so
// check that the error message refers to the ret and not the mov
// CHECK: No scratch registers
// CHECK-NOT: mov
// CHECK: ret
