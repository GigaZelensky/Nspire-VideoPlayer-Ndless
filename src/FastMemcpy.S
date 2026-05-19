.global FastMemcpy
@ void FastMemcpy(void* dest, const void* src, uint32_t chunks_32byte)
@ dest and src must be aligned
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
