/*
 * H.265 video codec.
 * Copyright (c) 2013-2014 struktur AG, Dirk Farin <farin@struktur.de>
 *
 * This file is part of libde265.
 *
 * libde265 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libde265 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libde265.  If not, see <http://www.gnu.org/licenses/>.
 */

/* ARM926 packed-context CABAC, byte-bounded refill and scalar EOF behavior. */
.syntax unified
.arm
.text
.align 5
.global ndless_decode_CABAC_bit
.type ndless_decode_CABAC_bit,%function
ndless_decode_CABAC_bit:
    push {r4,r5,lr}
    ldrb r2,[r1]
    ldr r3,[r0,#12]
    ldr r4,[r0,#16]
    ldr r5,=ndless_cabac_tables
    mov r12,r2,lsr #1
    add r12,r5,r12,lsl #2
    add r12,r12,r3,lsr #6
    ldrb r12,[r12,#-4]
    sub r3,r3,r12
    cmp r4,r3,lsl #7
    bhs .Lcabac_lps
    add lr,r5,r2
    ldrb lr,[lr,#256]
    strb lr,[r1]
    cmp r3,#256
    strhs r3,[r0,#12]
    andhs r0,r2,#1
    pophs {r4,r5,pc}
    ldrsh lr,[r0,#20]
    mov r3,r3,lsl #1
    mov r4,r4,lsl #1
    adds lr,lr,#1
    bne .Lcabac_store
    ldmib r0,{r1,r5}
    cmp r1,r5
    ldrblo r5,[r1],#1
    strlo r1,[r0,#4]
    orrlo r4,r4,r5
    mvn lr,#7
    b .Lcabac_store
.Lcabac_lps:
    add lr,r5,r2
    ldrb lr,[lr,#384]
    strb lr,[r1]
    add r5,r5,r12,lsr #3
    ldrb r5,[r5,#512]
    sub r4,r4,r3,lsl #7
    mov r3,r12,lsl r5
    mov r4,r4,lsl r5
    ldrsh lr,[r0,#20]
    adds lr,lr,r5
    eor r2,r2,#1
    bmi .Lcabac_store
    ldmib r0,{r1,r5}
    cmp r1,r5
    ldrblo r5,[r1],#1
    strlo r1,[r0,#4]
    orrlo r4,r4,r5,lsl lr
    sub lr,lr,#8
.Lcabac_store:
    str r3,[r0,#12]
    str r4,[r0,#16]
    strh lr,[r0,#20]
    and r0,r2,#1
    pop {r4,r5,pc}
.size ndless_decode_CABAC_bit,.-ndless_decode_CABAC_bit

/* Decode a sub-block's AC significance flags with CABAC state in registers.
 * r0 decoder, r1 significance models, r2 context map, r3 scan offsets;
 * stack: last scan position (0..15), output metadata. Returns output count.
 * The fixed-context extension path stays in C. */
.text
.align 5
.global ndless_decode_significant_coeffs
.type ndless_decode_significant_coeffs,%function
ndless_decode_significant_coeffs:
    push {r4-r11,lr}
    sub sp,sp,#20
    str r0,[sp]
    mov r10,r1
    mov lr,r2
    mov r12,r3
    ldr r11,[sp,#56]
    ldr r1,[sp,#60]
    str r1,[sp,#4]
    str r1,[sp,#8]
    ldr r4,[r0,#12]
    ldr r5,[r0,#16]
    ldrsh r6,[r0,#20]
    ldmib r0,{r7,r8}
    ldr r9,=ndless_cabac_tables
    cmp r11,#0
    ble .Lsig_done
.Lsig_loop:
    ldrb r0,[r12,r11]
    ldrb r0,[lr,r0]
    add r0,r10,r0
    ldrb r1,[r0]
    mov r2,r1,lsr #1
    add r2,r9,r2,lsl #2
    add r2,r2,r4,lsr #6
    ldrb r2,[r2,#-4]
    sub r4,r4,r2
    cmp r5,r4,lsl #7
    bhs .Lsig_lps
    add r3,r9,r1
    ldrb r3,[r3,#256]
    strb r3,[r0]
    cmp r4,#256
    bhs .Lsig_output
    mov r4,r4,lsl #1
    mov r5,r5,lsl #1
    adds r6,r6,#1
    bne .Lsig_output
    cmp r7,r8
    ldrblo r3,[r7],#1
    orrlo r5,r5,r3
    mvn r6,#7
    b .Lsig_output
.Lsig_lps:
    add r3,r9,r1
    ldrb r3,[r3,#384]
    strb r3,[r0]
    add r3,r9,r2,lsr #3
    ldrb r3,[r3,#512]
    sub r5,r5,r4,lsl #7
    mov r4,r2,lsl r3
    mov r5,r5,lsl r3
    adds r6,r6,r3
    eor r1,r1,#1
    bmi .Lsig_output
    cmp r7,r8
    ldrblo r3,[r7],#1
    orrlo r5,r5,r3,lsl r6
    sub r6,r6,#8
.Lsig_output:
    tst r1,#1
    ldrne r0,[sp,#8]
    orrne r3,r11,#0x50
    strbne r3,[r0],#1
    strne r0,[sp,#8]
    subs r11,r11,#1
    bgt .Lsig_loop
.Lsig_done:
    ldr r0,[sp]
    str r4,[r0,#12]
    str r5,[r0,#16]
    strh r6,[r0,#20]
    str r7,[r0,#4]
    ldr r0,[sp,#8]
    ldr r1,[sp,#4]
    sub r0,r0,r1
    add sp,sp,#20
    pop {r4-r11,pc}
.size ndless_decode_significant_coeffs,.-ndless_decode_significant_coeffs

/* Decode up to eight greater1 flags in one context group.
 * r0 decoder, r1 four-model group, r2 metadata, r3 count (0..8).
 * Return (final_c1<<4) | (first_greater1_index+1), with zero for no index.
 * c1 always starts at one, as required immediately after ctxSet selection. */
.text
.align 5
.global ndless_decode_greater1_coeffs
.type ndless_decode_greater1_coeffs,%function
ndless_decode_greater1_coeffs:
    push {r4-r11,lr}
    sub sp,sp,#20
    str r0,[sp]
    str r2,[sp,#4]
    mov r10,r1
    mov r11,r2
    mov lr,r3
    mov r12,#1
    mov r1,#0
    str r1,[sp,#8]
    ldr r4,[r0,#12]
    ldr r5,[r0,#16]
    ldrsh r6,[r0,#20]
    ldmib r0,{r7,r8}
    ldr r9,=ndless_cabac_tables
    cmp lr,#0
    ble .Lg1_done
.Lg1_loop:
    add r0,r10,r12
    ldrb r1,[r0]
    mov r2,r1,lsr #1
    add r2,r9,r2,lsl #2
    add r2,r2,r4,lsr #6
    ldrb r2,[r2,#-4]
    sub r4,r4,r2
    cmp r5,r4,lsl #7
    bhs .Lg1_lps
    add r3,r9,r1
    ldrb r3,[r3,#256]
    strb r3,[r0]
    cmp r4,#256
    bhs .Lg1_output
    mov r4,r4,lsl #1
    mov r5,r5,lsl #1
    adds r6,r6,#1
    bne .Lg1_output
    cmp r7,r8
    ldrblo r3,[r7],#1
    orrlo r5,r5,r3
    mvn r6,#7
    b .Lg1_output
.Lg1_lps:
    add r3,r9,r1
    ldrb r3,[r3,#384]
    strb r3,[r0]
    add r3,r9,r2,lsr #3
    ldrb r3,[r3,#512]
    sub r5,r5,r4,lsl #7
    mov r4,r2,lsl r3
    mov r5,r5,lsl r3
    adds r6,r6,r3
    eor r1,r1,#1
    bmi .Lg1_output
    cmp r7,r8
    ldrblo r3,[r7],#1
    orrlo r5,r5,r3,lsl r6
    sub r6,r6,#8
.Lg1_output:
    ldrb r0,[r11]
    tst r1,#1
    beq .Lg1_zero
    add r0,r0,#0x10
    mov r12,#0
    ldr r2,[sp,#8]
    cmp r2,#0
    streq r11,[sp,#8]
    b .Lg1_emit
.Lg1_zero:
    bic r0,r0,#0x40
    cmp r12,#0
    beq .Lg1_emit
    cmp r12,#3
    addlo r12,r12,#1
.Lg1_emit:
    strb r0,[r11],#1
    subs lr,lr,#1
    bne .Lg1_loop
.Lg1_done:
    ldr r0,[sp]
    str r4,[r0,#12]
    str r5,[r0,#16]
    strh r6,[r0,#20]
    str r7,[r0,#4]
    ldr r0,[sp,#8]
    ldr r1,[sp,#4]
    cmp r0,#0
    subne r0,r0,r1
    addne r0,r0,#1
    orr r0,r0,r12,lsl #4
    add sp,sp,#20
    pop {r4-r11,pc}
.size ndless_decode_greater1_coeffs,.-ndless_decode_greater1_coeffs

.section .note.GNU-stack,"",%progbits
