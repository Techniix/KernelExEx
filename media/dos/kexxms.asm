; KEXXMS.SYS - KernelExEx: XMS block sizes in whole pages, so Windows 98 can use
; more than 1 GB of RAM. Copyright (C) 2026 KernelExEx contributors. GPLv2.
;
; Load it right AFTER HIMEM.SYS in CONFIG.SYS:
;     DEVICE=C:\WINDOWS\HIMEM.SYS
;     DEVICE=C:\KEXXMS.SYS
;
; Why: Windows 98's loader takes extended memory from the XMS driver in blocks of
; the "largest free block" size. With XMS 2.0 that is at most 65535 KB, which is not
; a whole number of 4 KB pages, so every 64 MB block ends and the next begins in
; the middle of a page. VMM (its memory manager's start-up) leaves those pages out
; and so sees one memory range per 64 MB block; its table of ranges has 16 entries,
; so with more than 1 GB the ranges no longer fit and Windows cannot start
; ("Insufficient memory to initialize Windows", or a reset). Found with QEMU's
; debugger on Windows 98 SE's own VMM; see media/README.md.
; This driver passes every XMS call to HIMEM unchanged, except that it rounds the
; "largest free block" answers (functions 08h and 88h) down to whole 4 KB pages.
; The blocks then lie back to back, page-aligned, and VMM sees one range.
;
; Build: nasm -f bin -o KEXXMS.SYS kexxms.asm

bits 16
org 0

hdr:    dd -1
        dw 0x8000                       ; character device
        dw strategy
        dw interrupt
        db 'KEXXMS$ '

req:    dd 0
old2f:  dd 0
xms:    dd 0                            ; HIMEM's entry point
hnd:    dw 0
addr:   dw 0                            ; low word of a block's address
padded: db 0

; ---------------------------------------------------------------- resident part
; int 2Fh: AX=4310h (get the XMS entry point) returns ours
int2f:
        cmp ax, 0x4310
        je .entry
        jmp far [cs:old2f]
.entry: pushf
        call far [cs:old2f]             ; HIMEM: ES:BX = its entry point
        mov [cs:xms], bx
        mov [cs:xms+2], es
        push cs
        pop es
        mov bx, xmsentry
        iret

; The XMS entry point (called with CALL FAR). XMS drivers start with a short jump
; and two NOPs (some programs hook them by patching that); keep the shape.
xmsentry:
        jmp short .go
        nop
        nop
        nop
.go:    cmp ah, 0x08
        je .f08
        cmp ah, 0x88
        je .f88
        cmp ah, 0x09
        je .f09
        jmp far [cs:xms]
; 09h: allocate DX KB. Blocks that do not start on a page boundary are freed and
; allocated again behind a small padding block, so this and the following blocks
; start on page boundaries (they lie back to back, in whole pages).
.f09:   push dx                         ; the size wanted
        call far [cs:xms]
        pop cx
        test ax, ax
        jz .ret                         ; failed: as HIMEM said
        push bx
        mov [cs:hnd], dx
        mov ah, 0x0C                    ; lock: DX:BX = its address
        call far [cs:xms]
        test ax, ax
        jz .keep
        mov [cs:addr], bx
        mov dx, [cs:hnd]
        mov ah, 0x0D                    ; unlock
        call far [cs:xms]
        mov ax, [cs:addr]
        and ax, 0x0FFF
        jz .keep                        ; on a page boundary already
        cmp byte [cs:padded], 0
        jne .keep                       ; padded once already: leave it
        mov byte [cs:padded], 1
        push cx
        mov dx, [cs:hnd]
        mov ah, 0x0A                    ; free it
        call far [cs:xms]
        mov dx, 0x1000                  ; padding = (4096 - offset) / 1024 KB
        sub dx, [cs:addr]
        and dx, 0x0FFF
        shr dx, 10
        mov ah, 0x09                    ; (its handle is kept for good)
        call far [cs:xms]
        pop dx                          ; the size wanted, again
        mov ah, 0x09
        call far [cs:xms]
        test ax, ax
        jz .fail
        pop bx
        retf
.fail:  add sp, 2                       ; BL = HIMEM's error code
        retf
.keep:  mov dx, [cs:hnd]
        mov ax, 1
        pop bx
        retf
.f08:   call far [cs:xms]               ; AX = largest free block (KB), DX = total
        test ax, ax
        jz .ret
        and ax, 0xFFFC                  ; whole 4 KB pages
.ret:   retf
.f88:   call far [cs:xms]               ; EAX = largest free block (KB), EDX = total
        test eax, eax
        jz .ret
        and eax, 0xFFFFFFFC
        retf

resident_end:

; ---------------------------------------------------------------- driver entry
strategy:
        mov [cs:req], bx
        mov [cs:req+2], es
        retf

interrupt:
        pushf
        push ds
        push es
        pusha
        push cs
        pop ds
        les bx, [req]
        cmp byte [es:bx+2], 0           ; INIT only
        jne .done
        call init
.done:  les bx, [req]
        mov word [es:bx+3], 0x0100      ; done, no error
        popa
        pop es
        pop ds
        popf
        retf

; ---------------------------------------------------------------- INIT (discarded)
init:
        mov ax, 0x4300                  ; an XMS driver loaded?
        int 0x2F
        cmp al, 0x80
        jne .noxms
        xor ax, ax
        push es
        mov es, ax
        cli
        mov ax, [es:0x2F*4]
        mov [old2f], ax
        mov ax, [es:0x2F*4+2]
        mov [old2f+2], ax
        mov word [es:0x2F*4], int2f
        mov [es:0x2F*4+2], cs
        sti
        pop es
        mov dx, msg_ok
        mov ah, 9
        int 0x21
        les bx, [req]
        mov word [es:bx+14], resident_end
        mov [es:bx+16], cs
        ret
.noxms: mov dx, msg_noxms
        mov ah, 9
        int 0x21
        les bx, [req]
        mov word [es:bx+14], 0          ; don't stay resident
        mov [es:bx+16], cs
        ret

msg_ok:    db 'KEXXMS: XMS blocks in whole pages', 13, 10, '$'
msg_noxms: db 'KEXXMS: no XMS driver (load KEXXMS.SYS after HIMEM.SYS)', 13, 10, '$'
