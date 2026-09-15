/* SPDX-License-Identifier: MIT */
.section .multiboot,"a"
.align 4
.long 0x1badb002
.long 0x00000003
.long -(0x1badb002 + 0x00000003)
.section .text
.global _start
_start:
    cli
    mov $stack_top, %esp
    push %ebx
    call guest_main
1:  hlt
    jmp 1b
.section .bss
.align 16
stack_bottom:
    .skip 16384
stack_top:
.section .note.GNU-stack,"",@progbits
