/* Ndless scalar Main8 motion output, LGPL-3.0-or-later.
 * HEVC coefficients and rounding match fallback-motion.cpp.
 */
#include "fallback-motion.h"
#include "util.h"
#include <assert.h>

// ARMv5TE kernels keep each sliding filter window in registers. The luma
// loops rotate eight sample registers and test for completion every four
// outputs, covering all legal HEVC inter-prediction block dimensions.
// Scratch is ordinary signed16 data, with at least4-byte alignment.
#if defined(__arm__)
__asm__(R"ARM(
.macro ND_DOT7 a,b,c,d,e,f,g
    rsb lr,\a,\b,lsl #2
    sub lr,lr,\c,lsl #3
    sub lr,lr,\c,lsl #1
    add lr,lr,\d,lsl #6
    sub lr,lr,\d,lsl #2
    sub lr,lr,\d,lsl #1
    add lr,lr,\e,lsl #4
    add lr,lr,\e
    sub lr,lr,\f,lsl #2
    sub lr,lr,\f
    add lr,lr,\g
.endm
.macro ND_DOT8 a,b,c,d,e,f,g,h
    add lr,\d,\e
    add lr,lr,lr,lsl #2
    mov lr,lr,lsl #3
    add r2,\c,\f
    sub lr,lr,r2,lsl #3
    sub lr,lr,r2,lsl #1
    sub lr,lr,r2
    add r2,\b,\g
    add lr,lr,r2,lsl #2
    sub lr,lr,\a
    sub lr,lr,\h
.endm
.macro ND_CLIP_STORE8 rounding,shift
    add lr,lr,#\rounding
    mov lr,lr,asr #\shift
    cmp lr,#255
    movhi lr,lr,asr #31
    mvnhi lr,lr
    strb lr,[r0],r12
.endm
)ARM");
#endif


#if defined(__arm__)
extern "C" void ndless_epel_2d_arm(uint8_t*,ptrdiff_t,const uint8_t*,ptrdiff_t,
                                  int,int,int,int,int16_t*);
__asm__(R"ARM(
.syntax unified
.arm
.text
.align 2
.global ndless_epel_2d_arm
.type ndless_epel_2d_arm, %function
ndless_epel_2d_arm:
    push {r4-r11,lr}
    sub sp,sp,#28
    str r0,[sp,#0]
    str r1,[sp,#4]
    ldr r4,[sp,#64]
    ldr r5,[sp,#68]
    ldr r6,[sp,#80]
    str r4,[sp,#8]
    str r5,[sp,#12]
    str r6,[sp,#16]
    sub r0,r2,r3
    sub r0,r0,#1
    mov r1,r6
    sub lr,r3,r4
    sub lr,lr,#3
    add r3,r5,#3
    ldr r6,[sp,#72]
    adr r7,.Lepel_coefficients
    add r7,r7,r6,lsl #3
    ldm r7,{r8,r9}
.Lepel_h_row:
    ldrb r4,[r0],#1
    ldrb r5,[r0],#1
    ldrb r6,[r0],#1
    ldr r2,[sp,#8]
.Lepel_h_pair:
    ldrb r7,[r0],#1
    smulbb r11,r4,r8
    smlabt r11,r5,r8,r11
    smlabb r11,r6,r9,r11
    smlabt r11,r7,r9,r11
    ldrb r10,[r0],#1
    smulbb r12,r5,r8
    smlabt r12,r6,r8,r12
    smlabb r12,r7,r9,r12
    smlabt r12,r10,r9,r12
    strh r11,[r1],#2
    strh r12,[r1],#2
    mov r4,r6
    mov r5,r7
    mov r6,r10
    subs r2,r2,#2
    bne .Lepel_h_pair
    add r0,r0,lr
    subs r3,r3,#1
    bne .Lepel_h_row
    ldr r6,[sp,#76]
    adr r7,.Lepel_coefficients
    add r7,r7,r6,lsl #3
    ldm r7,{r8,r9}
    ldr r3,[sp,#8]
    mov lr,r3,lsr #1
    mov r3,r3,lsl #1
    ldr r12,[sp,#4]
.Lepel_v_column:
    ldr r0,[sp,#16]
    ldr r1,[sp,#0]
    ldr r2,[sp,#12]
    ldr r4,[r0],r3
    ldr r5,[r0],r3
    ldr r6,[r0],r3
.Lepel_v_row:
    ldr r7,[r0],r3
    smulbb r10,r4,r8
    smlabt r10,r5,r8,r10
    smlabb r10,r6,r9,r10
    smlabt r10,r7,r9,r10
    smultb r11,r4,r8
    smlatt r11,r5,r8,r11
    smlatb r11,r6,r9,r11
    smlatt r11,r7,r9,r11
    add r10,r10,#2048
    add r11,r11,#2048
    mov r10,r10,asr #12
    mov r11,r11,asr #12
    cmp r10,#255
    movhi r10,r10,asr #31
    mvnhi r10,r10
    cmp r11,#255
    movhi r11,r11,asr #31
    mvnhi r11,r11
    strb r10,[r1]
    strb r11,[r1,#1]
    add r1,r1,r12
    mov r4,r5
    mov r5,r6
    mov r6,r7
    subs r2,r2,#1
    bne .Lepel_v_row
    ldr r0,[sp,#16]
    ldr r1,[sp,#0]
    add r0,r0,#4
    add r1,r1,#2
    str r0,[sp,#16]
    str r1,[sp,#0]
    subs lr,lr,#1
    bne .Lepel_v_column
    add sp,sp,#28
    pop {r4-r11,pc}
.align 2
.Lepel_coefficients:
    .word 0x00400000,0x00000000
    .word 0x003afffe,0xfffe000a
    .word 0x0036fffc,0xfffe0010
    .word 0x002efffa,0xfffc001c
    .word 0x0024fffc,0xfffc0024
    .word 0x001cfffc,0xfffa002e
    .word 0x0010fffe,0xfffc0036
    .word 0x000afffe,0xfffe003a
.size ndless_epel_2d_arm, .-ndless_epel_2d_arm
)ARM");
#endif


__attribute__((noinline)) void put_unweighted_epel_2d_8_ndless(uint8_t* dst,ptrdiff_t dst_stride,
    const uint8_t* src,ptrdiff_t src_stride,int width,int height,int mx,int my,int16_t* scratch)
{
#if defined(__arm__)
  ndless_epel_2d_arm(dst,dst_stride,src,src_stride,width,height,mx,my,scratch);
#else
  assert(mx>0 && mx<8 && my>0 && my<8);
  static const int8_t taps[8][4] = {
    {0,64,0,0}, {-2,58,10,-2}, {-4,54,16,-2}, {-6,46,28,-4},
    {-4,36,36,-4}, {-4,28,46,-6}, {-2,16,54,-4}, {-2,10,58,-2}
  };
  const int8_t* h=taps[mx];
  const int h0=h[0], h1=h[1], h2=h[2], h3=h[3];
  for (int y=0;y<height+3;y++) {
    const uint8_t* p=src+(y-1)*src_stride-1;
    int16_t* out=scratch+y*width;
    for (int x=0;x<width;x++)
      out[x]=h0*p[x]+h1*p[x+1]+h2*p[x+2]+h3*p[x+3];
  }
  const int8_t* v=taps[my];
  const int v0=v[0], v1=v[1], v2=v[2], v3=v[3];
  for (int y=0;y<height;y++) {
    const int16_t* p=scratch+y*width;
    uint8_t* out=dst+y*dst_stride;
    for (int x=0;x<width;x++) {
      const int sum=v0*p[x]+v1*p[x+width]+v2*p[x+2*width]+v3*p[x+3*width];
      // The four-tap result fits int16 before output rounding. Combining the
      // two exact shifts avoids writing/rereading a full prediction plane.
      out[x]=Clip1_8bit((sum+2048)>>12);
    }
  }
#endif
}

#if defined(__arm__)
extern "C" void ndless_luma_axis_arm(uint8_t*,ptrdiff_t,const uint8_t*,ptrdiff_t,int,int,int,bool);
__asm__(R"ARM(
.syntax unified
.arm
.text
.align 2
.global ndless_luma_axis_arm
.type ndless_luma_axis_arm, %function
ndless_luma_axis_arm:
    push {r4-r11,lr}
    sub sp,sp,#36
    str r0,[sp,#0]
    ldr r4,[sp,#72]
    ldr r5,[sp,#76]
    ldr r6,[sp,#80]
    ldr r7,[sp,#84]
    str r6,[sp,#28]
    cmp r7,#0
    beq .Laxis_horizontal_setup
    mov r12,r1
    mov r7,#1
    str r7,[sp,#8]
    str r7,[sp,#12]
    str r5,[sp,#16]
    str r4,[sp,#20]
    b .Laxis_start
.Laxis_horizontal_setup:
    str r1,[sp,#8]
    str r3,[sp,#12]
    mov r12,#1
    mov r3,#1
    str r4,[sp,#16]
    str r5,[sp,#20]
.Laxis_start:
    cmp r6,#3
    sub r2,r2,r3,lsl #1
    subne r2,r2,r3
    str r2,[sp,#4]
.Laxis_outer:
    ldr r0,[sp,#0]
    ldr r1,[sp,#4]
    ldr r2,[sp,#16]
    str r2,[sp,#24]
    ldr r2,[sp,#28]
    ldrb r4,[r1],r3
    ldrb r5,[r1],r3
    ldrb r6,[r1],r3
    ldrb r7,[r1],r3
    ldrb r8,[r1],r3
    ldrb r9,[r1],r3
    cmp r2,#1
    beq .Laxis_phase1
    cmp r2,#3
    beq .Laxis_phase3
    ldrb r10,[r1],r3
    b .Laxis_phase2

.Laxis_phase1:
    ldrb r10,[r1],r3
    ND_DOT7 r4,r5,r6,r7,r8,r9,r10
    ND_CLIP_STORE8 32,6
    ldrb r11,[r1],r3
    ND_DOT7 r5,r6,r7,r8,r9,r10,r11
    ND_CLIP_STORE8 32,6
    ldrb r4,[r1],r3
    ND_DOT7 r6,r7,r8,r9,r10,r11,r4
    ND_CLIP_STORE8 32,6
    ldrb r5,[r1],r3
    ND_DOT7 r7,r8,r9,r10,r11,r4,r5
    ND_CLIP_STORE8 32,6
    ldr r2,[sp,#24]
    subs r2,r2,#4
    str r2,[sp,#24]
    beq .Laxis_next_outer
    ldrb r6,[r1],r3
    ND_DOT7 r8,r9,r10,r11,r4,r5,r6
    ND_CLIP_STORE8 32,6
    ldrb r7,[r1],r3
    ND_DOT7 r9,r10,r11,r4,r5,r6,r7
    ND_CLIP_STORE8 32,6
    ldrb r8,[r1],r3
    ND_DOT7 r10,r11,r4,r5,r6,r7,r8
    ND_CLIP_STORE8 32,6
    ldrb r9,[r1],r3
    ND_DOT7 r11,r4,r5,r6,r7,r8,r9
    ND_CLIP_STORE8 32,6
    ldr r2,[sp,#24]
    subs r2,r2,#4
    str r2,[sp,#24]
    beq .Laxis_next_outer
    b .Laxis_phase1
.Laxis_phase2:
    ldrb r11,[r1],r3
    ND_DOT8 r4,r5,r6,r7,r8,r9,r10,r11
    ND_CLIP_STORE8 32,6
    ldrb r4,[r1],r3
    ND_DOT8 r5,r6,r7,r8,r9,r10,r11,r4
    ND_CLIP_STORE8 32,6
    ldrb r5,[r1],r3
    ND_DOT8 r6,r7,r8,r9,r10,r11,r4,r5
    ND_CLIP_STORE8 32,6
    ldrb r6,[r1],r3
    ND_DOT8 r7,r8,r9,r10,r11,r4,r5,r6
    ND_CLIP_STORE8 32,6
    ldr r2,[sp,#24]
    subs r2,r2,#4
    str r2,[sp,#24]
    beq .Laxis_next_outer
    ldrb r7,[r1],r3
    ND_DOT8 r8,r9,r10,r11,r4,r5,r6,r7
    ND_CLIP_STORE8 32,6
    ldrb r8,[r1],r3
    ND_DOT8 r9,r10,r11,r4,r5,r6,r7,r8
    ND_CLIP_STORE8 32,6
    ldrb r9,[r1],r3
    ND_DOT8 r10,r11,r4,r5,r6,r7,r8,r9
    ND_CLIP_STORE8 32,6
    ldrb r10,[r1],r3
    ND_DOT8 r11,r4,r5,r6,r7,r8,r9,r10
    ND_CLIP_STORE8 32,6
    ldr r2,[sp,#24]
    subs r2,r2,#4
    str r2,[sp,#24]
    beq .Laxis_next_outer
    b .Laxis_phase2
.Laxis_phase3:
    ldrb r10,[r1],r3
    ND_DOT7 r10,r9,r8,r7,r6,r5,r4
    ND_CLIP_STORE8 32,6
    ldrb r11,[r1],r3
    ND_DOT7 r11,r10,r9,r8,r7,r6,r5
    ND_CLIP_STORE8 32,6
    ldrb r4,[r1],r3
    ND_DOT7 r4,r11,r10,r9,r8,r7,r6
    ND_CLIP_STORE8 32,6
    ldrb r5,[r1],r3
    ND_DOT7 r5,r4,r11,r10,r9,r8,r7
    ND_CLIP_STORE8 32,6
    ldr r2,[sp,#24]
    subs r2,r2,#4
    str r2,[sp,#24]
    beq .Laxis_next_outer
    ldrb r6,[r1],r3
    ND_DOT7 r6,r5,r4,r11,r10,r9,r8
    ND_CLIP_STORE8 32,6
    ldrb r7,[r1],r3
    ND_DOT7 r7,r6,r5,r4,r11,r10,r9
    ND_CLIP_STORE8 32,6
    ldrb r8,[r1],r3
    ND_DOT7 r8,r7,r6,r5,r4,r11,r10
    ND_CLIP_STORE8 32,6
    ldrb r9,[r1],r3
    ND_DOT7 r9,r8,r7,r6,r5,r4,r11
    ND_CLIP_STORE8 32,6
    ldr r2,[sp,#24]
    subs r2,r2,#4
    str r2,[sp,#24]
    beq .Laxis_next_outer
    b .Laxis_phase3
.Laxis_next_outer:
    ldr r2,[sp,#20]
    subs r2,r2,#1
    str r2,[sp,#20]
    beq .Laxis_done
    ldr r0,[sp,#0]
    ldr r1,[sp,#4]
    ldr r4,[sp,#8]
    ldr r5,[sp,#12]
    add r0,r0,r4
    add r1,r1,r5
    str r0,[sp,#0]
    str r1,[sp,#4]
    b .Laxis_outer
.Laxis_done:
    add sp,sp,#36
    pop {r4-r11,pc}
.size ndless_luma_axis_arm, .-ndless_luma_axis_arm
)ARM");
#endif

template<int Phase, bool Chroma>
static __attribute__((noinline)) void output_axis(uint8_t* dst, ptrdiff_t dst_stride,
                        const uint8_t* src, ptrdiff_t src_stride,
                        int width,int height,ptrdiff_t step)
{
  for (int y=0;y<height;y++) {
    const uint8_t* p=src+y*src_stride;
    uint8_t* out=dst+y*dst_stride;
    for (int x=0;x<width;x++,p++) {
      int sum;
      if (Chroma) {
        if (Phase==1) sum=-2*p[-step]+58*p[0]+10*p[step]-2*p[2*step];
        else if (Phase==2) sum=-4*p[-step]+54*p[0]+16*p[step]-2*p[2*step];
        else if (Phase==3) sum=-6*p[-step]+46*p[0]+28*p[step]-4*p[2*step];
        else if (Phase==4) sum=-4*p[-step]+36*p[0]+36*p[step]-4*p[2*step];
        else if (Phase==5) sum=-4*p[-step]+28*p[0]+46*p[step]-6*p[2*step];
        else if (Phase==6) sum=-2*p[-step]+16*p[0]+54*p[step]-4*p[2*step];
        else sum=-2*p[-step]+10*p[0]+58*p[step]-2*p[2*step];
      }
      else {
        if (Phase==1) sum=-p[-3*step]+4*p[-2*step]-10*p[-step]+58*p[0]+17*p[step]-5*p[2*step]+p[3*step];
        else if (Phase==2) sum=-p[-3*step]+4*p[-2*step]-11*p[-step]+40*p[0]+40*p[step]-11*p[2*step]+4*p[3*step]-p[4*step];
        else sum=p[-2*step]-5*p[-step]+17*p[0]+58*p[step]-10*p[2*step]+4*p[3*step]-p[4*step];
      }
      out[x]=Clip1_8bit((sum+32)>>6);
    }
  }
}

void put_unweighted_axis_8_ndless(uint8_t* dst,ptrdiff_t dst_stride,
                                 const uint8_t* src,ptrdiff_t src_stride,
                                 int width,int height,int phase,bool chroma,bool vertical)
{
#if defined(__arm__)
  if (!chroma) {
    ndless_luma_axis_arm(dst,dst_stride,src,src_stride,width,height,phase,vertical);
    return;
  }
#endif
  const ptrdiff_t step=vertical ? src_stride : 1;
#define PHASE_CASE(N,C) case N: output_axis<N,C>(dst,dst_stride,src,src_stride,width,height,step); break
  if (chroma) {
    switch(phase) {
      PHASE_CASE(1,true); PHASE_CASE(2,true); PHASE_CASE(3,true); PHASE_CASE(4,true);
      PHASE_CASE(5,true); PHASE_CASE(6,true); PHASE_CASE(7,true);
    }
  }
  else {
    switch(phase) { PHASE_CASE(1,false); PHASE_CASE(2,false); PHASE_CASE(3,false); }
  }
#undef PHASE_CASE
}

#if defined(__arm__)
extern "C" void ndless_qpel_h_arm(uint8_t*,ptrdiff_t,const uint8_t*,ptrdiff_t,int,int,int);
__asm__(R"ARM(
.syntax unified
.arm
.text
.align 2
.global ndless_qpel_h_arm
.type ndless_qpel_h_arm, %function
ndless_qpel_h_arm:
    push {r4-r11,lr}
    sub sp,sp,#12
    ldr r4,[sp,#48]
    ldr r12,[sp,#52]
    ldr r5,[sp,#56]
    cmp r5,#3
    sub r2,r2,#2
    subne r2,r2,#1
    mov r1,r2
    sub r3,r3,r4
    cmp r5,#2
    subeq r3,r3,#7
    subne r3,r3,#6
    str r3,[sp,#0]
    mov r4,r4,lsl #1
    str r4,[sp,#4]
    add r3,r0,r4
    cmp r5,#1
    beq .Lqpelh_init1
    cmp r5,#3
    beq .Lqpelh_init3
    b .Lqpelh_init2

.Lqpelh_init1:
    ldrb r4,[r1],#1
    ldrb r5,[r1],#1
    ldrb r6,[r1],#1
    ldrb r7,[r1],#1
    ldrb r8,[r1],#1
    ldrb r9,[r1],#1
.Lqpelh_phase1:
    ldrb r10,[r1],#1
    ND_DOT7 r4,r5,r6,r7,r8,r9,r10
    strh lr,[r0],#2
    ldrb r11,[r1],#1
    ND_DOT7 r5,r6,r7,r8,r9,r10,r11
    strh lr,[r0],#2
    ldrb r4,[r1],#1
    ND_DOT7 r6,r7,r8,r9,r10,r11,r4
    strh lr,[r0],#2
    ldrb r5,[r1],#1
    ND_DOT7 r7,r8,r9,r10,r11,r4,r5
    strh lr,[r0],#2
    cmp r0,r3
    beq .Lqpelh_next1
    ldrb r6,[r1],#1
    ND_DOT7 r8,r9,r10,r11,r4,r5,r6
    strh lr,[r0],#2
    ldrb r7,[r1],#1
    ND_DOT7 r9,r10,r11,r4,r5,r6,r7
    strh lr,[r0],#2
    ldrb r8,[r1],#1
    ND_DOT7 r10,r11,r4,r5,r6,r7,r8
    strh lr,[r0],#2
    ldrb r9,[r1],#1
    ND_DOT7 r11,r4,r5,r6,r7,r8,r9
    strh lr,[r0],#2
    cmp r0,r3
    beq .Lqpelh_next1
    b .Lqpelh_phase1

.Lqpelh_next1:
    subs r12,r12,#1
    beq .Lqpelh_done
    ldr r2,[sp,#0]
    add r1,r1,r2
    ldr r2,[sp,#4]
    add r3,r0,r2
    b .Lqpelh_init1
.Lqpelh_init2:
    ldrb r4,[r1],#1
    ldrb r5,[r1],#1
    ldrb r6,[r1],#1
    ldrb r7,[r1],#1
    ldrb r8,[r1],#1
    ldrb r9,[r1],#1
    ldrb r10,[r1],#1
.Lqpelh_phase2:
    ldrb r11,[r1],#1
    ND_DOT8 r4,r5,r6,r7,r8,r9,r10,r11
    strh lr,[r0],#2
    ldrb r4,[r1],#1
    ND_DOT8 r5,r6,r7,r8,r9,r10,r11,r4
    strh lr,[r0],#2
    ldrb r5,[r1],#1
    ND_DOT8 r6,r7,r8,r9,r10,r11,r4,r5
    strh lr,[r0],#2
    ldrb r6,[r1],#1
    ND_DOT8 r7,r8,r9,r10,r11,r4,r5,r6
    strh lr,[r0],#2
    cmp r0,r3
    beq .Lqpelh_next2
    ldrb r7,[r1],#1
    ND_DOT8 r8,r9,r10,r11,r4,r5,r6,r7
    strh lr,[r0],#2
    ldrb r8,[r1],#1
    ND_DOT8 r9,r10,r11,r4,r5,r6,r7,r8
    strh lr,[r0],#2
    ldrb r9,[r1],#1
    ND_DOT8 r10,r11,r4,r5,r6,r7,r8,r9
    strh lr,[r0],#2
    ldrb r10,[r1],#1
    ND_DOT8 r11,r4,r5,r6,r7,r8,r9,r10
    strh lr,[r0],#2
    cmp r0,r3
    beq .Lqpelh_next2
    b .Lqpelh_phase2

.Lqpelh_next2:
    subs r12,r12,#1
    beq .Lqpelh_done
    ldr r2,[sp,#0]
    add r1,r1,r2
    ldr r2,[sp,#4]
    add r3,r0,r2
    b .Lqpelh_init2
.Lqpelh_init3:
    ldrb r4,[r1],#1
    ldrb r5,[r1],#1
    ldrb r6,[r1],#1
    ldrb r7,[r1],#1
    ldrb r8,[r1],#1
    ldrb r9,[r1],#1
.Lqpelh_phase3:
    ldrb r10,[r1],#1
    ND_DOT7 r10,r9,r8,r7,r6,r5,r4
    strh lr,[r0],#2
    ldrb r11,[r1],#1
    ND_DOT7 r11,r10,r9,r8,r7,r6,r5
    strh lr,[r0],#2
    ldrb r4,[r1],#1
    ND_DOT7 r4,r11,r10,r9,r8,r7,r6
    strh lr,[r0],#2
    ldrb r5,[r1],#1
    ND_DOT7 r5,r4,r11,r10,r9,r8,r7
    strh lr,[r0],#2
    cmp r0,r3
    beq .Lqpelh_next3
    ldrb r6,[r1],#1
    ND_DOT7 r6,r5,r4,r11,r10,r9,r8
    strh lr,[r0],#2
    ldrb r7,[r1],#1
    ND_DOT7 r7,r6,r5,r4,r11,r10,r9
    strh lr,[r0],#2
    ldrb r8,[r1],#1
    ND_DOT7 r8,r7,r6,r5,r4,r11,r10
    strh lr,[r0],#2
    ldrb r9,[r1],#1
    ND_DOT7 r9,r8,r7,r6,r5,r4,r11
    strh lr,[r0],#2
    cmp r0,r3
    beq .Lqpelh_next3
    b .Lqpelh_phase3

.Lqpelh_next3:
    subs r12,r12,#1
    beq .Lqpelh_done
    ldr r2,[sp,#0]
    add r1,r1,r2
    ldr r2,[sp,#4]
    add r3,r0,r2
    b .Lqpelh_init3
.Lqpelh_done:
    add sp,sp,#12
    pop {r4-r11,pc}
.size ndless_qpel_h_arm, .-ndless_qpel_h_arm
)ARM");
#endif

#if defined(__arm__)
extern "C" void ndless_qpel_v_arm(uint8_t*,ptrdiff_t,const uint8_t*,ptrdiff_t,int,int,int);
__asm__(R"ARM(
.syntax unified
.arm
.text
.align 2
.global ndless_qpel_v_arm
.type ndless_qpel_v_arm, %function
ndless_qpel_v_arm:
    push {r4-r11,lr}
    sub sp,sp,#36
    str r0,[sp,#0]
    ldr r4,[sp,#72]
    ldr r5,[sp,#76]
    ldr r6,[sp,#80]
    mov r7,#1
    str r6,[sp,#28]
    cmp r7,#0
    beq .Lqpelv_horizontal_setup
    mov r12,r1
    mov r7,#1
    str r7,[sp,#8]
    mov r7,#2
    str r7,[sp,#12]
    mla r5,r1,r5,r0
    str r5,[sp,#16]
    str r4,[sp,#20]
    b .Lqpelv_start
.Lqpelv_horizontal_setup:
    str r1,[sp,#8]
    str r3,[sp,#12]
    mov r12,#1
    mov r3,#1
    str r4,[sp,#16]
    str r5,[sp,#20]
.Lqpelv_start:
    cmp r6,#3
    sub r2,r2,r3,lsl #1
    subne r2,r2,r3
    str r2,[sp,#4]
.Lqpelv_outer:
    ldr r0,[sp,#0]
    ldr r1,[sp,#4]
    ldr r2,[sp,#28]
    ldrsh r4,[r1],r3
    ldrsh r5,[r1],r3
    ldrsh r6,[r1],r3
    ldrsh r7,[r1],r3
    ldrsh r8,[r1],r3
    ldrsh r9,[r1],r3
    cmp r2,#1
    beq .Lqpelv_init1
    cmp r2,#3
    beq .Lqpelv_init3
    ldrsh r10,[r1],r3
    cmp r2,#4
    beq .Lqpelv_phase4
    b .Lqpelv_phase2

.Lqpelv_init1:
    ldr r2,[sp,#16]
.Lqpelv_phase1:
    ldrsh r10,[r1],r3
    ND_DOT7 r4,r5,r6,r7,r8,r9,r10
    ND_CLIP_STORE8 2048,12
    ldrsh r11,[r1],r3
    ND_DOT7 r5,r6,r7,r8,r9,r10,r11
    ND_CLIP_STORE8 2048,12
    ldrsh r4,[r1],r3
    ND_DOT7 r6,r7,r8,r9,r10,r11,r4
    ND_CLIP_STORE8 2048,12
    ldrsh r5,[r1],r3
    ND_DOT7 r7,r8,r9,r10,r11,r4,r5
    ND_CLIP_STORE8 2048,12
    cmp r0,r2
    beq .Lqpelv_next_outer
    ldrsh r6,[r1],r3
    ND_DOT7 r8,r9,r10,r11,r4,r5,r6
    ND_CLIP_STORE8 2048,12
    ldrsh r7,[r1],r3
    ND_DOT7 r9,r10,r11,r4,r5,r6,r7
    ND_CLIP_STORE8 2048,12
    ldrsh r8,[r1],r3
    ND_DOT7 r10,r11,r4,r5,r6,r7,r8
    ND_CLIP_STORE8 2048,12
    ldrsh r9,[r1],r3
    ND_DOT7 r11,r4,r5,r6,r7,r8,r9
    ND_CLIP_STORE8 2048,12
    cmp r0,r2
    beq .Lqpelv_next_outer
    b .Lqpelv_phase1
.Lqpelv_phase2:
    ldrsh r11,[r1],r3
    ND_DOT8 r4,r5,r6,r7,r8,r9,r10,r11
    ND_CLIP_STORE8 2048,12
    ldrsh r4,[r1],r3
    ND_DOT8 r5,r6,r7,r8,r9,r10,r11,r4
    ND_CLIP_STORE8 2048,12
    ldrsh r5,[r1],r3
    ND_DOT8 r6,r7,r8,r9,r10,r11,r4,r5
    ND_CLIP_STORE8 2048,12
    ldrsh r6,[r1],r3
    ND_DOT8 r7,r8,r9,r10,r11,r4,r5,r6
    ND_CLIP_STORE8 2048,12
    ldr r2,[sp,#16]
    cmp r0,r2
    beq .Lqpelv_next_outer
    ldrsh r7,[r1],r3
    ND_DOT8 r8,r9,r10,r11,r4,r5,r6,r7
    ND_CLIP_STORE8 2048,12
    ldrsh r8,[r1],r3
    ND_DOT8 r9,r10,r11,r4,r5,r6,r7,r8
    ND_CLIP_STORE8 2048,12
    ldrsh r9,[r1],r3
    ND_DOT8 r10,r11,r4,r5,r6,r7,r8,r9
    ND_CLIP_STORE8 2048,12
    ldrsh r10,[r1],r3
    ND_DOT8 r11,r4,r5,r6,r7,r8,r9,r10
    ND_CLIP_STORE8 2048,12
    ldr r2,[sp,#16]
    cmp r0,r2
    beq .Lqpelv_next_outer
    b .Lqpelv_phase2
.Lqpelv_init3:
    ldr r2,[sp,#16]
.Lqpelv_phase3:
    ldrsh r10,[r1],r3
    ND_DOT7 r10,r9,r8,r7,r6,r5,r4
    ND_CLIP_STORE8 2048,12
    ldrsh r11,[r1],r3
    ND_DOT7 r11,r10,r9,r8,r7,r6,r5
    ND_CLIP_STORE8 2048,12
    ldrsh r4,[r1],r3
    ND_DOT7 r4,r11,r10,r9,r8,r7,r6
    ND_CLIP_STORE8 2048,12
    ldrsh r5,[r1],r3
    ND_DOT7 r5,r4,r11,r10,r9,r8,r7
    ND_CLIP_STORE8 2048,12
    cmp r0,r2
    beq .Lqpelv_next_outer
    ldrsh r6,[r1],r3
    ND_DOT7 r6,r5,r4,r11,r10,r9,r8
    ND_CLIP_STORE8 2048,12
    ldrsh r7,[r1],r3
    ND_DOT7 r7,r6,r5,r4,r11,r10,r9
    ND_CLIP_STORE8 2048,12
    ldrsh r8,[r1],r3
    ND_DOT7 r8,r7,r6,r5,r4,r11,r10
    ND_CLIP_STORE8 2048,12
    ldrsh r9,[r1],r3
    ND_DOT7 r9,r8,r7,r6,r5,r4,r11
    ND_CLIP_STORE8 2048,12
    cmp r0,r2
    beq .Lqpelv_next_outer
    b .Lqpelv_phase3
.Lqpelv_phase4:
    ldrsh r11,[r1],r3
    ND_DOT8 r4,r5,r6,r7,r8,r9,r10,r11
    mov lr,lr,asr #6
    mov lr,lr,lsl #16
    mov lr,lr,asr #16
    ND_CLIP_STORE8 32,6
    ldrsh r4,[r1],r3
    ND_DOT8 r5,r6,r7,r8,r9,r10,r11,r4
    mov lr,lr,asr #6
    mov lr,lr,lsl #16
    mov lr,lr,asr #16
    ND_CLIP_STORE8 32,6
    ldrsh r5,[r1],r3
    ND_DOT8 r6,r7,r8,r9,r10,r11,r4,r5
    mov lr,lr,asr #6
    mov lr,lr,lsl #16
    mov lr,lr,asr #16
    ND_CLIP_STORE8 32,6
    ldrsh r6,[r1],r3
    ND_DOT8 r7,r8,r9,r10,r11,r4,r5,r6
    mov lr,lr,asr #6
    mov lr,lr,lsl #16
    mov lr,lr,asr #16
    ND_CLIP_STORE8 32,6
    ldr r2,[sp,#16]
    cmp r0,r2
    beq .Lqpelv_next_outer
    ldrsh r7,[r1],r3
    ND_DOT8 r8,r9,r10,r11,r4,r5,r6,r7
    mov lr,lr,asr #6
    mov lr,lr,lsl #16
    mov lr,lr,asr #16
    ND_CLIP_STORE8 32,6
    ldrsh r8,[r1],r3
    ND_DOT8 r9,r10,r11,r4,r5,r6,r7,r8
    mov lr,lr,asr #6
    mov lr,lr,lsl #16
    mov lr,lr,asr #16
    ND_CLIP_STORE8 32,6
    ldrsh r9,[r1],r3
    ND_DOT8 r10,r11,r4,r5,r6,r7,r8,r9
    mov lr,lr,asr #6
    mov lr,lr,lsl #16
    mov lr,lr,asr #16
    ND_CLIP_STORE8 32,6
    ldrsh r10,[r1],r3
    ND_DOT8 r11,r4,r5,r6,r7,r8,r9,r10
    mov lr,lr,asr #6
    mov lr,lr,lsl #16
    mov lr,lr,asr #16
    ND_CLIP_STORE8 32,6
    ldr r2,[sp,#16]
    cmp r0,r2
    beq .Lqpelv_next_outer
    b .Lqpelv_phase4
.Lqpelv_next_outer:
    ldr r2,[sp,#20]
    subs r2,r2,#1
    str r2,[sp,#20]
    beq .Lqpelv_done
    ldr r0,[sp,#0]
    ldr r1,[sp,#4]
    ldr r4,[sp,#8]
    ldr r5,[sp,#12]
    add r0,r0,r4
    add r1,r1,r5
    str r0,[sp,#0]
    str r1,[sp,#4]
    ldr r2,[sp,#16]
    add r2,r2,#1
    str r2,[sp,#16]
    b .Lqpelv_outer
.Lqpelv_done:
    add sp,sp,#36
    pop {r4-r11,pc}
.size ndless_qpel_v_arm, .-ndless_qpel_v_arm
)ARM");
#endif


void put_unweighted_qpel_2d_8_ndless(uint8_t* dst,ptrdiff_t dst_stride,
    const uint8_t* src,ptrdiff_t src_stride,int width,int height,int mx,int my,int16_t* scratch)
{
  assert(mx>0 && mx<4 && my>0 && my<4 && !(width&3) && !(height&3));
  const int before=my==3 ? 2 : 3, rows=height+(my==2 ? 7 : 6);
#if defined(__arm__)
  ndless_qpel_h_arm(reinterpret_cast<uint8_t*>(scratch),width*2,
                   src-before*src_stride,src_stride,width,rows,mx);
  ndless_qpel_v_arm(dst,dst_stride,reinterpret_cast<const uint8_t*>(scratch+before*width),
                   width*2,width,height,my+((mx==2 && my==2)?2:0));
#else
  static const int taps[3][8]={
    {-1,4,-10,58,17,-5,1,0}, {-1,4,-11,40,40,-11,4,-1}, {0,1,-5,17,58,-10,4,-1}};
  const int* h=taps[mx-1];const int* v=taps[my-1];
  for(int y=0;y<rows;y++)for(int x=0;x<width;x++) {
    const uint8_t* p=src+(y-before)*src_stride+x;int value=0;
    for(int k=0;k<8;k++)if(h[k])value+=h[k]*p[k-3];
    scratch[y*width+x]=value;
  }
  for(int y=0;y<height;y++)for(int x=0;x<width;x++) {
    const int16_t* p=scratch+(y+before)*width+x;int value=0;
    for(int k=0;k<8;k++)if(v[k])value+=v[k]*p[(k-3)*width];
    const int16_t prediction=value>>6;
    dst[y*dst_stride+x]=Clip1_8bit((prediction+32)>>6);
  }
#endif
}
