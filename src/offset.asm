; Same API as offset.c. Windows x64 ABI: RCX=text, RDX=logical index.
; Leaf function; preserves nonvolatile registers and does not touch the stack.
.code
PUBLIC doc_offset
doc_offset PROC
    xor eax, eax
again:
    test rdx, rdx
    jz done
    movzx r8d, word ptr [rcx+rax*2]
    test r8d, r8d
    jz outside
    inc eax
    cmp r8d, 13
    jne surrogate
    cmp word ptr [rcx+rax*2], 10
    je extra
    jmp next
surrogate:
    sub r8d, 0d800h
    cmp r8d, 03ffh
    ja next
    movzx r8d, word ptr [rcx+rax*2]
    sub r8d, 0dc00h
    cmp r8d, 03ffh
    ja next
extra:
    inc eax
next:
    dec rdx
    jmp again
outside:
    or eax, -1
done:
    ret
doc_offset ENDP
END
