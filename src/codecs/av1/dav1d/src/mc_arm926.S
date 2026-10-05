/* ARM926 four/six/eight-tap AV1 interpolation, Main 8-bit.
 * Intermediate rows have stride 128 and word alignment. Width is positive/even.
 * SPDX-License-Identifier: BSD-2-Clause */
.syntax unified
.arm
.text


/* dst, src, source stride, width; stack: height, filter, [destination stride].
 * The last argument is needed only for direct 8-bit output. The intermediate
 * form writes signed 16-bit samples with a fixed stride of 128 elements. */
/* Rotate the sample-register roles across pairs instead of moving the
 * complete window after every two output pixels. */
.macro horizontal_pair taps,direct,a,b,c
.if \direct
    mov r10,#34
    mov r11,#34
.else
    mov r10,#2
    mov r11,#2
.endif
    ldrb r12,[r1],#1
    smlabb r10,\a,r4,r10
    smlatt r10,\a,r4,r10
    smlabb r10,\b,r5,r10
    smlatt r10,\b,r5,r10
.if \taps == 6
    smlabb r10,\c,r6,r10
    smlatt r10,\c,r6,r10
.endif
    smlatb r11,\a,r4,r11
    smlabt r11,\b,r4,r11
    smlatb r11,\b,r5,r11
.if \taps == 6
    smlabt r11,\c,r5,r11
    smlatb r11,\c,r6,r11
    smlabt r11,r12,r6,r11
.else
    smlabt r11,r12,r5,r11
.endif
.if \direct
    mov r10,r10,asr #6
    mov r11,r11,asr #6
    bic r10,r10,r10,asr #31
    bic r11,r11,r11,asr #31
    cmp r10,#255
    movhi r10,#255
    cmp r11,#255
    movhi r11,#255
    strb r10,[r0],#1
    strb r11,[r0],#1
.else
    mov r10,r10,asr #2
    mov r11,r11,asr #2
    strh r10,[r0],#2
    strh r11,[r0],#2
.endif
.endm

.macro horizontal name, taps, direct, fixed=0
.align 5
.global \name
.type \name,%function
\name:
.if !\direct && !\fixed
.if \taps == 4
    cmp r3,#2
    beq dav1d_mc_h4_w2_8bpc_arm
    cmp r3,#4
    beq dav1d_mc_h4_w4_8bpc_arm
.else
    cmp r3,#8
    beq dav1d_mc_h6_w8_8bpc_arm
    cmp r3,#16
    beq dav1d_mc_h6_w16_8bpc_arm
.endif
.endif
    push {r4-r11,lr}
    ldr lr,[sp,#36]
    ldr r12,[sp,#40]
    sub sp,sp,#16
    str lr,[sp,#12]
    sub lr,r2,r3
    sub lr,lr,#(\taps-1)
    str lr,[sp,#4]
.if \direct
    mov lr,r3
    str lr,[sp,#8]
    ldr r10,[sp,#60]
    sub lr,r10,lr
.else
    mov lr,r3,lsl #1
    str lr,[sp,#8]
    rsb lr,lr,#256
.endif
    str lr,[sp]
    ldr r3,[sp,#12]
    ldrsb r4,[r12,#(4-\taps/2)]
    ldrsb r10,[r12,#(5-\taps/2)]
    mov r4,r4,lsl #16
    mov r4,r4,lsr #16
    orr r4,r4,r10,lsl #16
    ldrsb r5,[r12,#(6-\taps/2)]
    ldrsb r10,[r12,#(7-\taps/2)]
    mov r5,r5,lsl #16
    mov r5,r5,lsr #16
    orr r5,r5,r10,lsl #16
.if \taps == 6
    ldrsb r6,[r12,#5]
    ldrsb r10,[r12,#6]
    mov r6,r6,lsl #16
    mov r6,r6,lsr #16
    orr r6,r6,r10,lsl #16
.endif
    sub r1,r1,#(\taps/2-1)
1:
    ldrb r7,[r1],#1
    ldrb lr,[r1],#1
    orr r7,r7,lr,lsl #16
    ldrb r8,[r1],#1
    ldrb lr,[r1],#1
    orr r8,r8,lr,lsl #16
.if \taps == 6
    ldrb r9,[r1],#1
    ldrb lr,[r1],#1
    orr r9,r9,lr,lsl #16
.endif
.if \fixed
    .set hpair,0
    .rept (\fixed/2)
    .if \taps == 6
        .if (hpair%3) == 0
            horizontal_pair \taps,\direct,r7,r8,r9
        .elseif (hpair%3) == 1
            horizontal_pair \taps,\direct,r8,r9,r7
        .else
            horizontal_pair \taps,\direct,r9,r7,r8
        .endif
    .else
        .if (hpair%2) == 0
            horizontal_pair \taps,\direct,r7,r8,r9
        .else
            horizontal_pair \taps,\direct,r8,r7,r9
        .endif
    .endif
    .if hpair+1 < (\fixed/2)
        ldrb lr,[r1],#1
        .if \taps == 6
            .if (hpair%3) == 0
                orr r7,r12,lr,lsl #16
            .elseif (hpair%3) == 1
                orr r8,r12,lr,lsl #16
            .else
                orr r9,r12,lr,lsl #16
            .endif
        .else
            .if (hpair%2) == 0
                orr r7,r12,lr,lsl #16
            .else
                orr r8,r12,lr,lsl #16
            .endif
        .endif
    .endif
    .set hpair,hpair+1
    .endr
.else
    ldr lr,[sp,#8]
    add r2,r0,lr
2:
    horizontal_pair \taps,\direct,r7,r8,r9
    cmp r0,r2
    beq 3f
    ldrb lr,[r1],#1
    orr r7,r12,lr,lsl #16
.if \taps == 6
    horizontal_pair \taps,\direct,r8,r9,r7
    cmp r0,r2
    beq 3f
    ldrb lr,[r1],#1
    orr r8,r12,lr,lsl #16
    horizontal_pair \taps,\direct,r9,r7,r8
    cmp r0,r2
    beq 3f
    ldrb lr,[r1],#1
    orr r9,r12,lr,lsl #16
.else
    horizontal_pair \taps,\direct,r8,r7,r9
    cmp r0,r2
    beq 3f
    ldrb lr,[r1],#1
    orr r8,r12,lr,lsl #16
.endif
    b 2b
.endif
3:
    ldr lr,[sp]
    add r0,r0,lr
    ldr lr,[sp,#4]
    add r1,r1,lr
    subs r3,r3,#1
    bne 1b
    add sp,sp,#16
    pop {r4-r11,pc}
.size \name,.-\name
.endm

/* Halfword multiply instructions select neighboring samples directly from the
 * packed window. Each pair of outputs needs only two new input bytes. */
horizontal dav1d_mc_h4_8bpc_arm, 4, 0
horizontal dav1d_mc_h6_8bpc_arm, 6, 0
horizontal dav1d_mc_put_h4_8bpc_arm, 4, 1
horizontal dav1d_mc_put_h6_8bpc_arm, 6, 1

/* Common intermediate widths need no per-pair end tests. Dispatch above is
 * before saving registers; the same sample window and row strides are used. */
horizontal dav1d_mc_h4_w2_8bpc_arm, 4, 0, 2
horizontal dav1d_mc_h4_w4_8bpc_arm, 4, 0, 4
horizontal dav1d_mc_h6_w8_8bpc_arm, 6, 0, 8
horizontal dav1d_mc_h6_w16_8bpc_arm, 6, 0, 16

.macro load_filter_pair reg, off
    ldrsb \reg,[r12,#\off]
    ldrsb lr,[r12,#(\off+1)]
    mov \reg,\reg,lsl #16
    mov \reg,\reg,lsr #16
    orr \reg,\reg,lr,lsl #16
.endm

/* Two output rows share the N+1 packed input words. Row heights and pointer
 * increments remain outside the column loop; all four accumulators stay live. */
.macro vertical_madd tap, a, b
.if (\tap / 2) == 0
    .set vcoeff,4
.elseif (\tap / 2) == 1
    .set vcoeff,5
.elseif (\tap / 2) == 2
    .set vcoeff,6
.else
    .set vcoeff,7
.endif
.if vcoeff == 4
    vertical_madd_reg \tap,\a,\b,r4
.elseif vcoeff == 5
    vertical_madd_reg \tap,\a,\b,r5
.elseif vcoeff == 6
    vertical_madd_reg \tap,\a,\b,r6
.else
    vertical_madd_reg \tap,\a,\b,r7
.endif
.endm

.macro vertical_madd_reg tap, a, b, f
.if (\tap & 1)
    smlabt \a,r12,\f,\a
    smlatt \b,r12,\f,\b
.else
    smlabb \a,r12,\f,\a
    smlatb \b,r12,\f,\b
.endif
.endm

.macro vertical_store a,b,dst,prep
.if \prep
    mov \a,\a,asr #6
    mov \b,\b,asr #6
    strh \a,[\dst],#2
    strh \b,[\dst],#2
.else
    mov \a,\a,asr #10
    mov \b,\b,asr #10
    bic \a,\a,\a,asr #31
    bic \b,\b,\b,asr #31
    cmp \a,#255
    movhi \a,#255
    cmp \b,#255
    movhi \b,#255
    strb \a,[\dst],#1
    strb \b,[\dst],#1
.endif
.endm

.macro vertical_pair name,taps,prep=0
.align 5
.global \name
.type \name,%function
\name:
    push {r4-r11,lr}
    sub sp,sp,#16
    str r1,[sp,#12]
    mov r12,r3,lsl #1
    str r12,[sp,#8]
    rsb lr,r12,#512
    str lr,[sp,#4]
    mov lr,r1,lsl #1
.if \prep
    sub lr,lr,r3,lsl #1
.else
    sub lr,lr,r3
.endif
    str lr,[sp]
    add r1,r0,r1
    sub r2,r2,#(256*(\taps/2-1))
    ldr r12,[sp,#56]
    load_filter_pair r4,(4-\taps/2)
    load_filter_pair r5,(6-\taps/2)
.if \taps >= 6
    load_filter_pair r6,(8-\taps/2)
.endif
.if \taps == 8
    load_filter_pair r7,6
.endif
    ldr lr,[sp,#52]
    ldr r12,[sp,#12]
    cmp r12,#0
    rsblt r12,r12,#0
.if \prep
    cmp r12,r3,lsl #1
.else
    cmp r12,r3
.endif
    blo 6f
    cmp lr,#2
    blo 6f
4:
    ldr r12,[sp,#8]
    add r3,r2,r12
5:
.if \prep
    mov r8,#32
    mov r9,#32
    mov r10,#32
    mov r11,#32
.else
    mov r8,#512
    mov r9,#512
    mov r10,#512
    mov r11,#512
.endif
    .set vtap,0
    .rept (\taps+1)
    .if vtap == 0
        ldr r12,[r2],#4
    .else
        ldr r12,[r2,#(vtap*256-4)]
    .endif
    .if vtap < \taps
        vertical_madd vtap,r8,r9
    .endif
    .if vtap > 0
        vertical_madd (vtap-1),r10,r11
    .endif
    .set vtap,vtap+1
    .endr
    vertical_store r8,r9,r0,\prep
    vertical_store r10,r11,r1,\prep
    cmp r2,r3
    blo 5b
    ldr r12,[sp]
    add r0,r0,r12
    add r1,r1,r12
    ldr r12,[sp,#4]
    add r2,r2,r12
    subs lr,lr,#2
    beq 8f
    cmp lr,#2
    bhs 4b
6:
    ldr r12,[sp,#8]
    add r3,r2,r12
7:
.if \prep
    mov r8,#32
    mov r9,#32
.else
    mov r8,#512
    mov r9,#512
.endif
    .set vtap,0
    .rept \taps
    .if vtap == 0
        ldr r12,[r2],#4
    .else
        ldr r12,[r2,#(vtap*256-4)]
    .endif
    vertical_madd vtap,r8,r9
    .set vtap,vtap+1
    .endr
    vertical_store r8,r9,r0,\prep
    cmp r2,r3
    blo 7b
    subs lr,lr,#1
    beq 8f
    mov r0,r1
    ldr r12,[sp,#12]
    add r1,r1,r12
    ldr r12,[sp,#8]
    sub r2,r2,r12
    add r2,r2,#256
    b 6b
8:
    add sp,sp,#16
    pop {r4-r11,pc}
.size \name,.-\name
.endm

vertical_pair dav1d_mc_v4_8bpc_arm,4
vertical_pair dav1d_mc_v6_8bpc_arm,6
vertical_pair dav1d_mc_v8_8bpc_arm,8
vertical_pair dav1d_mc_prep_v4_8bpc_arm,4,1
vertical_pair dav1d_mc_prep_v6_8bpc_arm,6,1
vertical_pair dav1d_mc_prep_v8_8bpc_arm,8,1

/* Eight-tap sharp interpolation. Keep coefficients and the byte window in
 * packed halfwords, advancing by two samples for each pair of outputs. */


.macro horizontal8_pair direct,a,b,c,d
.if \direct
    mov r12,#34
.else
    mov r12,#2
.endif
    smlabb r12,\a,r4,r12
    smlatt r12,\a,r4,r12
    smlabb r12,\b,r5,r12
    smlatt r12,\b,r5,r12
    smlabb r12,\c,r6,r12
    smlatt r12,\c,r6,r12
    smlabb r12,\d,r7,r12
    smlatt r12,\d,r7,r12
.if \direct
    mov r12,r12,asr #6
    bic r12,r12,r12,asr #31
    cmp r12,#255
    movhi r12,#255
    strb r12,[r0],#1
    mov r12,#34
.else
    mov r12,r12,asr #2
    strh r12,[r0],#2
    mov r12,#2
.endif
    ldrb lr,[r1],#1
    smlatb r12,\a,r4,r12
    smlabt r12,\b,r4,r12
    smlatb r12,\b,r5,r12
    smlabt r12,\c,r5,r12
    smlatb r12,\c,r6,r12
    smlabt r12,\d,r6,r12
    smlatb r12,\d,r7,r12
    smlabt r12,lr,r7,r12
.if \direct
    mov r12,r12,asr #6
    bic r12,r12,r12,asr #31
    cmp r12,#255
    movhi r12,#255
    strb r12,[r0],#1
.else
    mov r12,r12,asr #2
    strh r12,[r0],#2
.endif
.endm

.macro horizontal8 name, direct
.align 5
.global \name
.type \name,%function
\name:
    push {r4-r11,lr}
    ldr lr,[sp,#36]
    ldr r12,[sp,#40]
    sub sp,sp,#16
    str lr,[sp,#12]
    sub lr,r2,r3
    sub lr,lr,#7
    str lr,[sp,#4]
.if \direct
    str r3,[sp,#8]
    ldr lr,[sp,#60]
    sub lr,lr,r3
.else
    mov lr,r3,lsl #1
    str lr,[sp,#8]
    rsb lr,lr,#256
.endif
    str lr,[sp]
    load_filter_pair r4,0
    load_filter_pair r5,2
    load_filter_pair r6,4
    load_filter_pair r7,6
    ldr r3,[sp,#12]
    sub r1,r1,#3
1:
    ldrb r8,[r1],#1
    ldrb lr,[r1],#1
    orr r8,r8,lr,lsl #16
    ldrb r9,[r1],#1
    ldrb lr,[r1],#1
    orr r9,r9,lr,lsl #16
    ldrb r10,[r1],#1
    ldrb lr,[r1],#1
    orr r10,r10,lr,lsl #16
    ldrb r11,[r1],#1
    ldrb lr,[r1],#1
    orr r11,r11,lr,lsl #16
    ldr lr,[sp,#8]
    add r2,r0,lr
2:
    horizontal8_pair \direct,r8,r9,r10,r11
    cmp r0,r2
    beq 3f
    ldrb r12,[r1],#1
    orr r8,lr,r12,lsl #16
    horizontal8_pair \direct,r9,r10,r11,r8
    cmp r0,r2
    beq 3f
    ldrb r12,[r1],#1
    orr r9,lr,r12,lsl #16
    horizontal8_pair \direct,r10,r11,r8,r9
    cmp r0,r2
    beq 3f
    ldrb r12,[r1],#1
    orr r10,lr,r12,lsl #16
    horizontal8_pair \direct,r11,r8,r9,r10
    cmp r0,r2
    beq 3f
    ldrb r12,[r1],#1
    orr r11,lr,r12,lsl #16
    b 2b
3:
    ldr lr,[sp]
    add r0,r0,lr
    ldr lr,[sp,#4]
    add r1,r1,lr
    subs r3,r3,#1
    bne 1b
    add sp,sp,#16
    pop {r4-r11,pc}
.size \name,.-\name
.endm

horizontal8 dav1d_mc_h8_8bpc_arm, 0
horizontal8 dav1d_mc_put_h8_8bpc_arm, 1

/* Direct vertical pixel output shares each byte across two row sums. */
.macro vertical_pixel_madd tap,a
.if (\tap / 2) == 0
    vertical_pixel_madd_reg \tap,\a,r4
.elseif (\tap / 2) == 1
    vertical_pixel_madd_reg \tap,\a,r5
.elseif (\tap / 2) == 2
    vertical_pixel_madd_reg \tap,\a,r6
.else
    vertical_pixel_madd_reg \tap,\a,r7
.endif
.endm
.macro vertical_pixel_madd_reg tap,a,f
.if (\tap & 1)
    smlabt \a,r12,\f,\a
.else
    smlabb \a,r12,\f,\a
.endif
.endm
.macro vertical_pixel_store a,dst
    mov \a,\a,asr #6
    bic \a,\a,\a,asr #31
    cmp \a,#255
    movhi \a,#255
    strb \a,[\dst],#1
.endm

.macro vertical_pixels_pair name,taps
.align 5
.global \name
.type \name,%function
\name:
    push {r4-r11,lr}
    sub sp,sp,#16
    str r1,[sp,#12]
    ldr r9,[sp,#52]
    str r9,[sp,#8]
    mov r8,r1,lsl #1
    sub r8,r8,r9
    str r8,[sp]
    mov r8,r3,lsl #1
    sub r8,r8,r9
    str r8,[sp,#4]
    add r1,r0,r1
    sub r2,r2,r3
.if \taps == 8
    sub r2,r2,r3,lsl #1
.elseif \taps == 6
    sub r2,r2,r3
.endif
    ldr r12,[sp,#60]
    load_filter_pair r4,(4-\taps/2)
    load_filter_pair r5,(6-\taps/2)
.if \taps >= 6
    load_filter_pair r6,(8-\taps/2)
.endif
.if \taps == 8
    load_filter_pair r7,6
.endif
    ldr lr,[sp,#56]
    ldr r12,[sp,#12]
    cmp r12,#0
    rsblt r12,r12,#0
    cmp r12,r9
    blo 6f
    cmp lr,#2
    blo 6f
4:
    ldr r12,[sp,#8]
    add r9,r0,r12
5:
    mov r8,r2
    mov r10,#32
    mov r11,#32
    .set vtap,0
    .rept (\taps+1)
        ldrb r12,[r8],r3
    .if vtap < \taps
        vertical_pixel_madd vtap,r10
    .endif
    .if vtap > 0
        vertical_pixel_madd (vtap-1),r11
    .endif
    .set vtap,vtap+1
    .endr
    vertical_pixel_store r10,r0
    vertical_pixel_store r11,r1
    add r2,r2,#1
    cmp r0,r9
    bne 5b
    ldr r12,[sp]
    add r0,r0,r12
    add r1,r1,r12
    ldr r12,[sp,#4]
    add r2,r2,r12
    subs lr,lr,#2
    beq 8f
    cmp lr,#2
    bhs 4b
6:
    ldr r12,[sp,#8]
    add r9,r0,r12
7:
    mov r8,r2
    mov r10,#32
    .set vtap,0
    .rept \taps
        ldrb r12,[r8],r3
        vertical_pixel_madd vtap,r10
    .set vtap,vtap+1
    .endr
    vertical_pixel_store r10,r0
    add r2,r2,#1
    cmp r0,r9
    bne 7b
    subs lr,lr,#1
    beq 8f
    mov r0,r1
    ldr r12,[sp,#12]
    add r1,r1,r12
    ldr r12,[sp,#8]
    sub r2,r2,r12
    add r2,r2,r3
    b 6b
8:
    add sp,sp,#16
    pop {r4-r11,pc}
.size \name,.-\name
.endm
vertical_pixels_pair dav1d_mc_put_v4_8bpc_arm,4
vertical_pixels_pair dav1d_mc_put_v6_8bpc_arm,6
vertical_pixels_pair dav1d_mc_put_v8_8bpc_arm,8

.section .note.GNU-stack,"",%progbits
