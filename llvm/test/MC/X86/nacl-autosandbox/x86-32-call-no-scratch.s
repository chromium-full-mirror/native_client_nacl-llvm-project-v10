// RUN: llvm-mc -filetype asm -triple i386-unknown-nacl %s | FileCheck %s

// Tests that ecx is used as the default scratch register for indirect calls
// when no other scratch register is specified and the flag
// -nacl-use-x86-call-convention is set to true (which is its default).

	call *(%ebx)
// CHECK:	movl (%ebx), %ecx
// CHECK-NEXT: 	.bundle_lock align_to_end
// CHECK-NEXT:  andl	$-32, %ecx
// CHECK-NEXT:  calll	*%ecx
// CHECK-NEXT:  .bundle_unlock

	call *12(%ebx)
// CHECK:	movl 12(%ebx), %ecx
// CHECK-NEXT: 	.bundle_lock align_to_end
// CHECK-NEXT:  andl	$-32, %ecx
// CHECK-NEXT:  calll	*%ecx
// CHECK-NEXT:  .bundle_unlock

	call *-12(%ebx,%edx)
// CHECK:	movl -12(%ebx,%edx), %ecx
// CHECK-NEXT: 	.bundle_lock align_to_end
// CHECK-NEXT:  andl	$-32, %ecx
// CHECK-NEXT:  calll	*%ecx
// CHECK-NEXT:  .bundle_unlock

	call *-12(%ebx,%edx,4)
// CHECK:	movl -12(%ebx,%edx,4), %ecx
// CHECK-NEXT: 	.bundle_lock align_to_end
// CHECK-NEXT:  andl	$-32, %ecx
// CHECK-NEXT:  calll	*%ecx
// CHECK-NEXT:  .bundle_unlock
