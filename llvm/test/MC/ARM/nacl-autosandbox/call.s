// RUN: llvm-mc -filetype asm -triple armv7-unknown-nacl %s | FileCheck %s

foo:

	bl foo
//CHECK:    .bundle_lock align_to_end
//CHECK-NEXT:    bl	foo
//CHECK-NEXT: 	.bundle_unlock

	blx r0
//CHECK:    .bundle_lock align_to_end
//CHECK-NEXT: 	bic	r0, r0, #-1073741809
//CHECK-NEXT: 	blx	r0
//CHECK-NEXT: 	.bundle_unlock

	blx sp
//CHECK:    .bundle_lock align_to_end
//CHECK-NEXT:    blx	sp
//CHECK-NEXT: 	.bundle_unlock

	blx pc
//CHECK:    .bundle_lock align_to_end
//CHECK-NEXT:    blx	pc
//CHECK-NEXT: 	.bundle_unlock


	blne foo
//CHECK:    .bundle_lock align_to_end
//CHECK-NEXT:    blne	foo
//CHECK-NEXT: 	.bundle_unlock

	blxne r0
//CHECK:    .bundle_lock align_to_end
//CHECK-NEXT: 	bicne	r0, r0, #-1073741809
//CHECK-NEXT: 	blxne	r0
//CHECK-NEXT: 	.bundle_unlock

	blxne sp
//CHECK:    .bundle_lock align_to_end
//CHECK-NEXT:    blxne	sp
//CHECK-NEXT: 	.bundle_unlock

	blxne pc
//CHECK:    .bundle_lock align_to_end
//CHECK-NEXT:    blxne	pc
//CHECK-NEXT: 	.bundle_unlock
