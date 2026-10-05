.global FastMemcpy
@ void FastMemcpy(void* dest, const void* src, uint32_t chunks_32byte)
@ dest and src must be 4-byte aligned; length is a count of 32-byte blocks
@ Copy loop credited to https://github.com/giraf-fe/nvid2
FastMemcpy:
    push {r4-r11, lr}
    @ r0 = dest
    @ r1 = src
    @ r2 = chunks_32byte
    @ r4-r11: data registers
    cmp r2, #0
    beq done
loop:
    ldmia r1!, {r4-r11}     @ load 32 bytes from src
    stmia r0!, {r4-r11}     @ store 32 bytes to dest
    subs r2, r2, #1         @ decrement chunk count
    bne loop

done:
    pop {r4-r11, pc}


/* Copy tightly packed destination rows from a padded source. Word-aligned
 * pointers/stride, non-overlapping buffers. r2 is 32-byte blocks per row,
 * r3 is row count; stack argument is source stride in bytes. */
.global FastMemcpyRows
.type FastMemcpyRows,%function
.align 5
FastMemcpyRows:
    cmp r2,#0
    cmpne r3,#0
    bxeq lr
    push {r4-r11,lr}
    ldr lr,[sp,#36]
    sub lr,lr,r2,lsl #5
    mov r12,r2
1:
    mov r2,r12
2:
    ldmia r1!,{r4-r11}
    stmia r0!,{r4-r11}
    subs r2,r2,#1
    bne 2b
    subs r3,r3,#1
    addne r1,r1,lr
    bne 1b
    pop {r4-r11,pc}
.size FastMemcpyRows,.-FastMemcpyRows
