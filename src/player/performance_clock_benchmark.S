/* Fixed register-only work for relative clock verification. Warm before
 * timing; the hot loop occupies one cache line and performs no data access. */
.syntax unified
.arm
.text
.align 5
.global performance_clock_benchmark_loop
.type performance_clock_benchmark_loop, %function
performance_clock_benchmark_loop:
    mov r1, #0
    .align 5
1:
    add r1, r1, #1
    add r1, r1, #1
    add r1, r1, #1
    add r1, r1, #1
    add r1, r1, #1
    add r1, r1, #1
    subs r0, r0, #1
    bne 1b
    bx lr
.size performance_clock_benchmark_loop, .-performance_clock_benchmark_loop
.section .note.GNU-stack,"",%progbits
