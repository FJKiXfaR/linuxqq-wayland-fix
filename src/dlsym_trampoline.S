/*
 * dlsym 蹦床。
 *
 * 先调用 qqwl_dlsym_decide(handle, name, 返回地址)：
 *   - 返回非 NULL：把它作为 dlsym 的结果返回；
 *   - 返回 NULL：恢复参数，尾跳转到真正的 dlsym。
 *
 * glibc 的 dlsym 根据返回地址判断调用者（dlsym(RTLD_NEXT, ...) 依赖这一点），
 * 尾跳转能让真 dlsym 看到的返回地址仍然是原调用者。
 */

#if defined(__x86_64__)

    .text
    .globl  dlsym
    .type   dlsym, @function
dlsym:
    .cfi_startproc
    endbr64
    pushq   %rdi
    .cfi_adjust_cfa_offset 8
    pushq   %rsi
    .cfi_adjust_cfa_offset 8
    subq    $8, %rsp                    /* 保持 16 字节对齐 */
    .cfi_adjust_cfa_offset 8
    movq    24(%rsp), %rdx              /* 第三个参数：原调用者的返回地址 */
    call    qqwl_dlsym_decide
    addq    $8, %rsp
    .cfi_adjust_cfa_offset -8
    popq    %rsi
    .cfi_adjust_cfa_offset -8
    popq    %rdi
    .cfi_adjust_cfa_offset -8
    testq   %rax, %rax
    jnz     1f
    jmp     *qqwl_real_dlsym(%rip)
1:
    ret
    .cfi_endproc
    .size   dlsym, .-dlsym

#elif defined(__aarch64__)

    .text
    .globl  dlsym
    .type   dlsym, %function
dlsym:
    .cfi_startproc
    hint    #34                         /* bti c */
    stp     x29, x30, [sp, #-32]!
    .cfi_def_cfa_offset 32
    .cfi_offset 29, -32
    .cfi_offset 30, -24
    mov     x29, sp
    stp     x0, x1, [sp, #16]
    mov     x2, x30                     /* 第三个参数：原调用者的返回地址 */
    bl      qqwl_dlsym_decide
    mov     x3, x0
    ldp     x0, x1, [sp, #16]
    ldp     x29, x30, [sp], #32
    .cfi_restore 29
    .cfi_restore 30
    .cfi_def_cfa_offset 0
    cbnz    x3, 1f
    adrp    x16, qqwl_real_dlsym
    ldr     x16, [x16, #:lo12:qqwl_real_dlsym]
    br      x16
1:
    mov     x0, x3
    ret
    .cfi_endproc
    .size   dlsym, .-dlsym

#endif

    .section .note.GNU-stack, "", %progbits
