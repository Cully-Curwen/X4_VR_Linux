EXTERN copy_fixture_original:PROC
PUBLIC copy_fixture_site
.code
copy_fixture PROC FRAME
    sub rsp, 28h
    .allocstack 28h
    .endprolog
copy_fixture_site LABEL BYTE
    call copy_fixture_original
    add rsp, 28h
    ret
copy_fixture ENDP
copy_fixture_other PROC FRAME
    sub rsp, 28h
    .allocstack 28h
    .endprolog
    call copy_fixture_original
    add rsp, 28h
    ret
copy_fixture_other ENDP
END
