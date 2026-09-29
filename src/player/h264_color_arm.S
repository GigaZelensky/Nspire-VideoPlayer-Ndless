/* Exact YUV420 -> RGB565 for a pair of dense rows (ARM926, little endian).
 * r0/r1: Y rows; r2/r3: U/V. Stack: dst0, dst1, y_base, pair count.
 * y_base is followed by four 256-entry chroma tables, then the clip table.
 * Caller supplies nonempty even width and word-aligned destination rows. */
.syntax unified
.arm
.text
.align 5
.global h264_rgb565_dense_pair_rows
.type h264_rgb565_dense_pair_rows, %function
h264_rgb565_dense_pair_rows:
    push {r4-r11, lr}
    ldr r4, [sp, #36]
    ldr r5, [sp, #40]
    ldr r6, [sp, #44]
    ldr r8, [sp, #48]
    add r7, r6, #5120
    add r8, r0, r8, lsl #1
    push {r8}
1:
    ldrb r12, [r2], #1
    add r11, r6, r12, lsl #2
    ldr r10, [r11, #1024]
    ldr r9, [r11, #2048]
    ldrb r12, [r3], #1
    add r11, r6, r12, lsl #2
    add r11, r11, #4096
    ldr r8, [r11, #-1024]
    ldr r12, [r11]
    add r9, r9, r12
    ldrb r12, [r0], #1
    ldr r11, [r6, r12, lsl #2]
    add r12, r11, r10
    ldrb lr, [r7, r12, asr #11]
    add r12, r11, r9
    ldrb r12, [r7, r12, asr #10]
    orr lr, lr, r12, lsl #5
    add r12, r11, r8
    ldrb r12, [r7, r12, asr #11]
    orr lr, lr, r12, lsl #11
    ldrb r12, [r0], #1
    ldr r11, [r6, r12, lsl #2]
    add r12, r11, r10
    ldrb r12, [r7, r12, asr #11]
    orr lr, lr, r12, lsl #16
    add r12, r11, r9
    ldrb r12, [r7, r12, asr #10]
    orr lr, lr, r12, lsl #21
    add r12, r11, r8
    ldrb r12, [r7, r12, asr #11]
    orr lr, lr, r12, lsl #27
    str lr, [r4], #4
    ldrb r12, [r1], #1
    ldr r11, [r6, r12, lsl #2]
    add r12, r11, r10
    ldrb lr, [r7, r12, asr #11]
    add r12, r11, r9
    ldrb r12, [r7, r12, asr #10]
    orr lr, lr, r12, lsl #5
    add r12, r11, r8
    ldrb r12, [r7, r12, asr #11]
    orr lr, lr, r12, lsl #11
    ldrb r12, [r1], #1
    ldr r11, [r6, r12, lsl #2]
    add r12, r11, r10
    ldrb r12, [r7, r12, asr #11]
    orr lr, lr, r12, lsl #16
    add r12, r11, r9
    ldrb r12, [r7, r12, asr #10]
    orr lr, lr, r12, lsl #21
    add r12, r11, r8
    ldrb r12, [r7, r12, asr #11]
    orr lr, lr, r12, lsl #27
    str lr, [r5], #4
    ldr r12, [sp]
    cmp r0, r12
    blo 1b
    add sp, sp, #4
    pop {r4-r11, pc}
.size h264_rgb565_dense_pair_rows, .-h264_rgb565_dense_pair_rows
.section .note.GNU-stack,"",%progbits
