/* this used to have llvm-mc -nacl-enable-auto-sandboxing -filetype asm -triple x86_64-unknown-nacl %s -sfi-hide-sandbox-base=false | FileCheck %s --check-prefix=X8664
*/
// RUN: llvm-mc -nacl-enable-auto-sandboxing -filetype asm -triple x86_64-unknown-nacl %s | FileCheck %s --check-prefix=HIDEBASE
// RUN: llvm-mc -nacl-enable-auto-sandboxing -filetype obj -triple x86_64-unknown-nacl --position-independent %s | llvm-objdump --disassemble - | FileCheck %s --check-prefix=PIC
/* Most likely we don't have to care about --position-independent, but this
 * test is weird for a different reason: NaCl should never be emitting call.
 */
.scratch %r11
foo:

	call foo
//HIDEBASE:	callq 	foo

//PIC: 		leal	25(%rip), %r10d
//PIC-NEXT: 	pushq	%r10
//PIC-NEXT: 	jmp	-14

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

//PIC: 		movq	%rax, %r11
//PIC-NEXT: 	leal	22(%rip), %r10d /* this is 17 instead of 22 */
//PIC-NEXT: 	pushq	%r10
//PIC-NEXT: 	andl	$-32, %r11d
//PIC-NEXT: 	addq	%r15, %r11
//PIC-NEXT: 	jmpq	*%r11


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

//PIC: 		movq	12(%rsp), %r11
//PIC-NEXT: 	leal	20(%rip), %r10d
//PIC-NEXT: 	pushq	%r10
//PIC-NEXT: 	andl	$-32, %r11d
//PIC-NEXT: 	addq	%r15, %r11
//PIC-NEXT: 	jmpq	*%r11


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

//PIC: 		leal	131072(%rax,%rdi,8), %r11d
//PIC-NEXT: 	movq	(%r15,%r11), %r11
//PIC-NEXT: 	leal	13(%rip), %r10d
//PIC-NEXT: 	pushq	%r10
//PIC-NEXT: 	andl	$-32, %r11d
//PIC-NEXT: 	addq	%r15, %r11
//PIC-NEXT: 	jmpq	*%r11
