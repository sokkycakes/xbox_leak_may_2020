/* Entry stack: export index, Xbox return address, stack arguments.
 * Preserve i386 callee-saved registers; realign before entering C. */
.text
.globl xbr_entry
.hidden xbr_entry
.type xbr_entry,@function
xbr_entry:
    pushl %ebp
    movl %esp, %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    andl $-16, %esp
    subl $64, %esp
    leal 32(%esp), %esi
    movl 4(%ebp), %eax
    movl %eax, 0(%esp)
    leal 12(%ebp), %eax
    movl %eax, 4(%esp)
    movl %ecx, 8(%esp)
    movl %edx, 12(%esp)
    movl %esi, 16(%esp)
    call xbr_do_call
    /* Relocate the return address past callee-cleaned arguments. cdecl
     * reports zero; fastcall counts only arguments read from the stack. */
    movl 20(%esi), %ecx
    leal 8(%ebp,%ecx), %ecx
    movl 8(%ebp), %ebx
    movl %ebx, (%ecx)
    movl 0(%esi), %eax
    movl 4(%esi), %edx
    cmpl $0, 16(%esi)
    je 1f
    fldl 8(%esi)
1:
    movl -4(%ebp), %ebx
    movl -8(%ebp), %esi
    movl -12(%ebp), %edi
    movl 0(%ebp), %ebp
    movl %ecx, %esp
    ret
.size xbr_entry, .-xbr_entry
.section .note.GNU-stack,"",@progbits
