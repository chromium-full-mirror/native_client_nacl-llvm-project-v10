// RUN: llvm-mc -filetype asm -triple x86_64-unknown-nacl %s | FileCheck %s --check-prefix=HIDEBASE

.scratch %r11
foo:

	call foo

//HIDEBASE:      pushq $.LDirectCallRetAddr{{[0-9]+}}
//HIDEBASE-NEXT: jmp foo
//HIDEBASE-NEXT: .p2align	5, 0x90
//HIDEBASE-NEXT: .LDirectCallRetAddr{{[0-9]+}}:

	call *%rax

//HIDEBASE:    	 movq	%rax, %r11
//HIDEBASE-NEXT: pushq	$.LIndirectCallRetAddr{{[0-9]+}}
//HIDEBASE-NEXT: .bundle_lock
//HIDEBASE-NEXT: andl	$-32, %r11d
//HIDEBASE-NEXT: addq	%r15, %r11
//HIDEBASE-NEXT: jmpq	*%r11
//HIDEBASE-NEXT: .bundle_unlock
//HIDEBASE-NEXT: .p2align	5, 0x90
//HIDEBASE-NEXT: .LIndirectCallRetAddr{{[0-9]+}}:

	call *12(%rsp)

//HIDEBASE:    	 movq	12(%rsp), %r11
//HIDEBASE-NEXT: pushq	$.LIndirectCallRetAddr{{[0-9]+}}
//HIDEBASE-NEXT: .bundle_lock
//HIDEBASE-NEXT: andl	$-32, %r11d
//HIDEBASE-NEXT: addq	%r15, %r11
//HIDEBASE-NEXT: jmpq	*%r11
//HIDEBASE-NEXT: .bundle_unlock
//HIDEBASE-NEXT: .p2align	5, 0x90
//HIDEBASE-NEXT: .LIndirectCallRetAddr{{[0-9]+}}

	call *131072(%rax, %rdi, 8)

//HIDEBASE:    	 .bundle_lock
//HIDEBASE-NEXT: leal	131072(%rax,%rdi,8), %r11d
//HIDEBASE-NEXT: movq	(%r15,%r11), %r11
//HIDEBASE-NEXT: .bundle_unlock
//HIDEBASE-NEXT: pushq	$.LIndirectCallRetAddr{{[0-9]+}}
//HIDEBASE-NEXT: .bundle_lock
//HIDEBASE-NEXT: andl	$-32, %r11d
//HIDEBASE-NEXT: addq	%r15, %r11
//HIDEBASE-NEXT: jmpq	*%r11
//HIDEBASE-NEXT: .bundle_unlock
//HIDEBASE-NEXT: .p2align	5, 0x90
//HIDEBASE-NEXT: .LIndirectCallRetAddr{{[0-9]+}}:
