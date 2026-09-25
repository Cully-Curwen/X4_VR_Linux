; Owned test code only. First ten bytes match the observed X4 pose-update
; prologue, exercising relocation of mov rax,rsp and sub rsp,168h. The rest
; is a trivial copy/counter/return fixture, NOT a copy of the X4 function.
.code
pose_fixture PROC FRAME
    mov rax, rsp
    sub rsp, 168h
    .allocstack 168h
    .endprolog
    movups xmm0, xmmword ptr [rdx]
    movups xmmword ptr [rcx], xmm0
    movups xmm1, xmmword ptr [rdx+10h]
    movups xmmword ptr [rcx+10h], xmm1
    movups xmm0, xmmword ptr [rdx+20h]
    movups xmmword ptr [rcx+20h], xmm0
    movups xmm1, xmmword ptr [rdx+30h]
    movups xmmword ptr [rcx+30h], xmm1
    inc qword ptr [rcx+40h]
    mov qword ptr [rcx+48h], rdx
    mov rax, rcx
    add rsp, 168h
    ret
pose_fixture ENDP
END
