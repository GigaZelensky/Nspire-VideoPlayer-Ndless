.syntax unified
.arch armv5te
.arm
.text
.align 2
.global private_context_swap
.type private_context_swap,%function
/* Only called at C call boundaries, with IRQ/FIQ already masked. Preserve
 * AAPCS callee-saved registers including r9, and eight-byte stack alignment.
 * r12 is a padding word. All stack metadata changes happen under that mask. */
private_context_swap:
    push {r4-r11, r12, lr}
    str sp, [r0]
    mov sp, r1
    pop {r4-r11, r12, pc}
.size private_context_swap,.-private_context_swap

.global private_context_entry
.type private_context_entry,%function
private_context_entry:
    mov r0, r4
    blx r5
1:  b 1b /* The C entry returns to the foreground with swap, never by return. */
.size private_context_entry,.-private_context_entry
.section .note.GNU-stack,"",%progbits
