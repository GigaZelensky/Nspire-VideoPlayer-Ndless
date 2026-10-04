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
.section .note.GNU-stack,"",%progbits
