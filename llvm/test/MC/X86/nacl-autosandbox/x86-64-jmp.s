/* this used to be
/* llvm-mc -filetype asm -triple x86_64-unknown-nacl %s -sfi-hide-sandbox-base=false | FileCheck %s --check-prefix=X8664
*/
// RUN: llvm-mc -filetype asm -triple x86_64-unknown-nacl %s | FileCheck %s --check-prefix=HIDEBASE
.scratch %r11
foo:

	jmp foo
//HIDEBASE:    	jmp	foo

	jmp *%rax
//HIDEBASE:    	 movq	%rax, %r11
//HIDEBASE-NEXT: .bundle_lock
//HIDEBASE-NEXT: andl	$-32, %r11d
//HIDEBASE-NEXT: addq	%r15, %r11
//HIDEBASE-NEXT: jmpq	*%r11
//HIDEBASE-NEXT: .bundle_unlock

	jmp *12(%rsp)
//HIDEBASE:    	 movq	12(%rsp), %r11
//HIDEBASE-NEXT: .bundle_lock
//HIDEBASE-NEXT: andl	$-32, %r11d
//HIDEBASE-NEXT: addq	%r15, %r11
//HIDEBASE-NEXT: jmpq	*%r11
//HIDEBASE-NEXT: .bundle_unloc

	jmp *131072(%rax, %rdi, 8)
//HIDEBASE:    	 .bundle_lock
//HIDEBASE-NEXT: leal	131072(%rax,%rdi,8), %r11d
//HIDEBASE-NEXT: movq	(%r15,%r11), %r11
//HIDEBASE-NEXT: .bundle_unlock
//HIDEBASE-NEXT: .bundle_lock
//HIDEBASE-NEXT: andl	$-32, %r11d
//HIDEBASE-NEXT: addq	%r15, %r11
//HIDEBASE-NEXT: jmpq	*%r11
//HIDEBASE-NEXT: .bundle_unlock
