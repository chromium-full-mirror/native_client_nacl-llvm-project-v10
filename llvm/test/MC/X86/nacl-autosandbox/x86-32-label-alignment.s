// RUN: llvm-mc %s -triple i386-unknown-nacl -filetype=obj -o - | llvm-objdump -d - | FileCheck %s

foo:
   call bar

// CHECK:  0 <foo>:

bar:
   call foo

// CHECK:  0 <bar>:
