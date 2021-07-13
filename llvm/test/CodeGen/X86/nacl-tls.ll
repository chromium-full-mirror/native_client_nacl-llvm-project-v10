; RUN: llc -mtriple=x86_64-unknown-nacl -filetype=asm -nacl-enable-autosandboxing=false %s -o - | FileCheck %s

@tls_var = external thread_local global i32

define i32* @get_tls_addr() {
  ret i32* @tls_var
}

; CHECK: get_tls_addr:
; CHECK: callq __nacl_read_tp
