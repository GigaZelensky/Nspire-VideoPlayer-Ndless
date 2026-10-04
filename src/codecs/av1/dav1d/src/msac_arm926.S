/* ARM926 AV1 range decoders.
 * SPDX-License-Identifier: BSD-2-Clause */
.syntax unified
.arm
.text
.align 5
.global dav1d_msac_decode_symbol_adapt3_arm926
.type dav1d_msac_decode_symbol_adapt3_arm926,%function
dav1d_msac_decode_symbol_adapt3_arm926:
    push {r4-r11,lr}
    ldr r3,[r0,#8]
    ldr r4,[r0,#12]
    ldrh r8,[r1]
    ldrh r9,[r1,#2]
    ldrh r10,[r1,#4]
    mov r5,r4,lsr #8
    mov r6,r3,lsr #16
    mov lr,r8,lsr #6
    mul r7,r5,lr
    mov r7,r7,lsr #1
    add r7,r7,#12
    cmp r6,r7
    movhs r11,#0
    bhs 1f
    mov r4,r7
    mov lr,r9,lsr #6
    mul r7,r5,lr
    mov r7,r7,lsr #1
    add r7,r7,#8
    cmp r6,r7
    movhs r11,#1
    bhs 1f
    mov r4,r7
    mov lr,r10,lsr #6
    mul r7,r5,lr
    mov r7,r7,lsr #1
    add r7,r7,#4
    cmp r6,r7
    movhs r11,#2
    movlo r11,#3
    movlo r4,r7
    movlo r7,#0
1:
    sub r3,r3,r7,lsl #16
    sub r4,r4,r7
    clz r2,r4
    sub r2,r2,#16
    mov r3,r3,lsl r2
    mov r4,r4,lsl r2
    ldr r5,[r0,#16]
    cmp r5,r2
    sub r5,r5,r2
    str r3,[r0,#8]
    str r4,[r0,#12]
    str r5,[r0,#16]
    blo 3f
2:
    ldr r2,[r0,#20]
    cmp r2,#0
    beq 5f
    ldrh r4,[r1,#6]
    mov r5,r4,lsr #4
    add r5,r5,#5
    cmp r11,#0
    rsbhi r2,r8,#32768
    addhi r8,r8,r2,lsr r5
    subls r8,r8,r8,lsr r5
    cmp r11,#1
    rsbhi r2,r9,#32768
    addhi r9,r9,r2,lsr r5
    subls r9,r9,r9,lsr r5
    cmp r11,#2
    rsbhi r2,r10,#32768
    addhi r10,r10,r2,lsr r5
    subls r10,r10,r10,lsr r5
    cmp r4,#32
    addlo r4,r4,#1
    strh r8,[r1]
    strh r9,[r1,#2]
    strh r10,[r1,#4]
    strh r4,[r1,#6]
5:
    mov r0,r11
    pop {r4-r11,pc}
3:
    ldr r4,[r0]
    ldr r7,[r0,#4]
    rsb r6,r5,#8
4:
    cmp r4,r7
    bhs 6f
    ldrb r2,[r4],#1
    eor r2,r2,#255
    orr r3,r3,r2,lsl r6
    subs r6,r6,#8
    bpl 4b
7:
    rsb r5,r6,#8
    str r4,[r0]
    str r3,[r0,#8]
    str r5,[r0,#16]
    b 2b
6:
    mvn r2,#255
    mvn r2,r2,lsl r6
    orr r3,r3,r2
    b 7b
.size dav1d_msac_decode_symbol_adapt3_arm926,.-dav1d_msac_decode_symbol_adapt3_arm926
/* Generic alphabet path. Halfword accesses also allow CDFs at a two-byte offset. */
.align 5
.global dav1d_msac_decode_symbol_adapt_arm926
.type dav1d_msac_decode_symbol_adapt_arm926,%function
dav1d_msac_decode_symbol_adapt_arm926:
    push {r4-r11,lr}
    ldr r3,[r0,#8]
    ldr r4,[r0,#12]
    mov r10,r2
    mov r8,r1
    mov r5,r4,lsr #8
    mov r6,r3,lsr #16
    mov r9,r2,lsl #2
    mov r11,#0
.Lgen_find:
    ldrh lr,[r8],#2
    mov lr,lr,lsr #6
    mul r7,r5,lr
    add r7,r9,r7,lsr #1
    cmp r6,r7
    bhs .Lgen_found
    mov r4,r7
    sub r9,r9,#4
    add r11,r11,#1
    b .Lgen_find
.Lgen_found:
    sub r3,r3,r7,lsl #16
    sub r4,r4,r7
    clz r2,r4
    sub r2,r2,#16
    mov r3,r3,lsl r2
    mov r4,r4,lsl r2
    ldr r5,[r0,#16]
    cmp r5,r2
    sub r5,r5,r2
    str r3,[r0,#8]
    str r4,[r0,#12]
    str r5,[r0,#16]
    blo .Lgen_refill
/* The unsigned refill test above matches the C decoder, including EOF. */
.Lgen_update:
    ldr r2,[r0,#20]
    cmp r2,#0
    beq .Lgen_done
    add r8,r1,r10,lsl #1
    ldrh r4,[r8]
    mov r5,r4,lsr #4
    add r5,r5,#4
    cmp r10,#2
    addhi r5,r5,#1
    cmp r4,#32
    addlo r4,r4,#1
    strh r4,[r8]
    /* Update entries on either side of the decoded symbol in pairs. */
    mov r8,r1
    tst r11,#1
    ldrhne r2,[r8]
    rsbne r6,r2,#32768
    addne r2,r2,r6,lsr r5
    strhne r2,[r8],#2
    movs r9,r11,lsr #1
    beq .Lgen_lower
.Lgen_upper_loop:
    ldrh r2,[r8]
    ldrh r4,[r8,#2]
    rsb r6,r2,#32768
    rsb r7,r4,#32768
    add r2,r2,r6,lsr r5
    add r4,r4,r7,lsr r5
    strh r2,[r8],#2
    strh r4,[r8],#2
    subs r9,r9,#1
    bne .Lgen_upper_loop
.Lgen_lower:
    sub r9,r10,r11
    tst r9,#1
    ldrhne r2,[r8]
    subne r2,r2,r2,lsr r5
    strhne r2,[r8],#2
    movs r9,r9,lsr #1
    beq .Lgen_done
.Lgen_lower_loop:
    ldrh r2,[r8]
    ldrh r4,[r8,#2]
    sub r2,r2,r2,lsr r5
    sub r4,r4,r4,lsr r5
    strh r2,[r8],#2
    strh r4,[r8],#2
    subs r9,r9,#1
    bne .Lgen_lower_loop
.Lgen_done:
    mov r0,r11
    pop {r4-r11,pc}
.Lgen_refill:
    ldr r4,[r0]
    ldr r7,[r0,#4]
    rsb r6,r5,#8
.Lgen_refill_loop:
    cmp r4,r7
    bhs .Lgen_eof
    ldrb r2,[r4],#1
    eor r2,r2,#255
    orr r3,r3,r2,lsl r6
    subs r6,r6,#8
    bpl .Lgen_refill_loop
.Lgen_refilled:
    rsb r5,r6,#8
    str r4,[r0]
    str r3,[r0,#8]
    str r5,[r0,#16]
    b .Lgen_update
.Lgen_eof:
    mvn r2,#255
    mvn r2,r2,lsl r6
    orr r3,r3,r2
    b .Lgen_refilled
.size dav1d_msac_decode_symbol_adapt_arm926,.-dav1d_msac_decode_symbol_adapt_arm926

/* Adaptive boolean: consume the CDF before renormalization so refill needs
 * only the input/context pointers. TST without a shift preserves carry from
 * the symbol decision for both conditional probability updates. */
.align 5
.global dav1d_msac_decode_bool_adapt_arm926
.type dav1d_msac_decode_bool_adapt_arm926,%function
dav1d_msac_decode_bool_adapt_arm926:
    push {r4,r5,r7,lr}
    ldr r3,[r0,#8]
    ldr r4,[r0,#12]
    ldrh r5,[r1]
    mov r2,r5,lsr #6
    mov r12,r4,lsr #8
    mul r2,r12,r2
    mov r2,r2,lsr #1
    add r2,r2,#4
    cmp r3,r2,lsl #16
    sbc r7,r7,r7
    subhs r3,r3,r2,lsl #16
    subhs r2,r4,r2
    ldr r12,[r0,#20]
    tst r12,r12
    beq .Lbool_norm
    ldrh r4,[r1,#2]
    mov r12,r4,lsr #4
    add r12,r12,#4
    rsblo lr,r5,#32768
    addlo r5,r5,lr,lsr r12
    subhs r5,r5,r5,lsr r12
    cmp r4,#32
    addlo r4,r4,#1
    strh r5,[r1]
    strh r4,[r1,#2]
.Lbool_norm:
    clz r12,r2
    sub r12,r12,#16
    mov r3,r3,lsl r12
    mov r4,r2,lsl r12
    ldr r5,[r0,#16]
    cmp r5,r12
    sub r5,r5,r12
    str r3,[r0,#8]!
    stmib r0,{r4,r5}
    blo .Lbool_refill
.Lbool_done:
    and r0,r7,#1
    pop {r4,r5,r7,pc}
.Lbool_refill:
    ldr r1,[r0,#-8]
    ldr r2,[r0,#-4]
    rsb r4,r5,#8
.Lbool_byte:
    cmp r1,r2
    bhs .Lbool_eof
    ldrb r5,[r1],#1
    eor r5,r5,#255
    orr r3,r3,r5,lsl r4
    subs r4,r4,#8
    bpl .Lbool_byte
.Lbool_refilled:
    rsb r5,r4,#8
    str r1,[r0,#-8]
    str r3,[r0]
    str r5,[r0,#8]
    b .Lbool_done
.Lbool_eof:
    mvn r5,#255
    mvn r5,r5,lsl r4
    orr r3,r3,r5
    b .Lbool_refilled
.size dav1d_msac_decode_bool_adapt_arm926,.-dav1d_msac_decode_bool_adapt_arm926

.section .note.GNU-stack,"",%progbits
