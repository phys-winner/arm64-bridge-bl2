.syntax unified
.arm
.fpu neon
.text
.align 2
.global NativeThunkEntry
.hidden NativeThunkEntry
.type NativeThunkEntry,%function
NativeThunkEntry:
    // Veneer reserved eight bytes and saved original R12 there.
    sub sp, sp, #72
    stmia sp, {r0-r11}
    str r12, [sp, #64]
    ldr r0, [sp, #72]
    str r0, [sp, #48]
    add r0, sp, #80
    str r0, [sp, #52]
    str lr, [sp, #56]
    str lr, [sp, #60]
    mrs r0, APSR
    str r0, [sp, #68]
    vpush {d0-d15}
    vpush {d16-d31}
    vmrs r1, FPSCR
    push {r1,r2}
    add r0, sp, #264
    bl NativeDispatchFrame
    pop {r1,r2}
    vmsr FPSCR, r1
    vpop {d16-d31}
    vpop {d0-d15}
    ldr r0, [sp, #68]
    msr APSR_nzcvqg, r0
    // Put the branch target in the top reserved word before restoring R0-R12.
    ldr r0, [sp, #60]
    str r0, [sp, #76]
    ldr lr, [sp, #56]
    ldmia sp, {r0-r12}
    add sp, sp, #76
    ldr pc, [sp], #4
.size NativeThunkEntry, .-NativeThunkEntry

.global NativeCallWords
.hidden NativeCallWords
.type NativeCallWords,%function
NativeCallWords:
    push {r4-r11,r12,lr}
    mov r4,r0
    mov r5,r1
    subs r6,r2,#4
    movmi r6,#0
    lsl r7,r6,#2
    add r7,r7,#7
    bic r7,r7,#7
    sub sp,sp,r7
    mov r8,#0
1:
    cmp r8,r6
    bge 2f
    add r9,r8,#4
    ldr r10,[r5,r9,lsl #2]
    str r10,[sp,r8,lsl #2]
    add r8,r8,#1
    b 1b
2:
    ldmia r5,{r0-r3}
    blx r4
    add sp,sp,r7
    pop {r4-r11,r12,pc}
.size NativeCallWords, .-NativeCallWords
.section .note.GNU-stack,"",%progbits
