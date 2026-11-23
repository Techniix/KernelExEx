; KernelExEx - KEXUNDO.COM: from the install stick, undo the "more than 1 GB" patch
; of a Windows on drive d: (KEXUNDO D:) when that Windows no longer starts:
;   d:\WINDOWS\SYSTEM\VMM32.KEX (the original) is copied over VMM32.VXD, and the
;   "/MAX=..." option of the KEXMEM.SYS line in d:\CONFIG.SYS is blanked out, so
;   KEXMEM limits Windows to 1 GB again.
; Exit code: 2 undone, 0 declined (/ASK), 1 failed.
; Copyright (C) 2026, KernelExEx contributors. GPLv2. Build: nasm -f bin -o KEXUNDO.COM
        bits 16
        org 0x100
BUFSZ   equ 0xC000

start:  cld
        mov si, 0x81                    ; the drive letter: first non-blank character
.skip:  lodsb
        cmp al, ' '
        je .skip
        cmp al, 13
        je usage
        and al, 0xDF                    ; upper case
        cmp al, 'C'
        jb usage
        cmp al, 'Z'
        ja usage
        mov [kexf], al
        mov [vxdf], al
        mov [cfgf], al
        ; "/ASK": ask first (the stick's start-up offers the undo this way)
.opt:   lodsb
        cmp al, 13
        je .go
        cmp al, '/'
        jne .opt
        mov dx, msg_ask
        call say
        mov ah, 8                       ; read a key, no echo
        int 0x21
        and al, 0xDF
        cmp al, 'U'
        je .yes
        mov dx, msg_crlf
        call say
        mov ax, 0x4C00
        int 0x21
.yes:   mov dx, msg_crlf
        call say
.go:
        ; 1. VMM32.KEX -> VMM32.VXD
        mov dx, kexf
        mov ax, 0x3D00
        int 0x21
        jc nokex
        mov [hin], ax
        mov dx, vxdf
        mov ax, 0x4301                  ; clear its attributes
        xor cx, cx
        int 0x21
        mov dx, vxdf
        mov ah, 0x3C                    ; create/truncate
        xor cx, cx
        int 0x21
        jc ioerr
        mov [hout], ax
.copy:  mov bx, [hin]
        mov ah, 0x3F
        mov cx, BUFSZ
        mov dx, buf
        int 0x21
        jc ioerr
        test ax, ax
        jz .copied
        mov cx, ax
        mov bx, [hout]
        mov ah, 0x40
        mov dx, buf
        int 0x21
        jc ioerr
        cmp ax, cx
        jne ioerr
        jmp .copy
.copied: mov bx, [hout]
        mov ah, 0x3E
        int 0x21
        mov bx, [hin]
        mov ah, 0x3E
        int 0x21
        mov dx, msg_vmm
        call say

        ; 2. CONFIG.SYS: blank out "/MAX=..." (up to the end of the word)
        mov dx, cfgf
        mov ax, 0x3D02                  ; read/write
        int 0x21
        jc .nocfg
        mov [hin], ax
        mov bx, ax
        mov ah, 0x3F
        mov cx, BUFSZ
        mov dx, buf
        int 0x21
        jc ioerr
        mov [len], ax
        mov di, buf
        mov cx, ax
        xor bp, bp                      ; changed?
.find:  cmp cx, 5
        jb .write
        mov si, di
        lodsb
        cmp al, '/'
        jne .next
        lodsb
        and al, 0xDF
        cmp al, 'M'
        jne .next
        lodsb
        and al, 0xDF
        cmp al, 'A'
        jne .next
        lodsb
        and al, 0xDF
        cmp al, 'X'
        jne .next
        ; blank until a space, tab or line end
        push cx
.blank: mov al, [di]
        cmp al, ' '
        je .bdone
        cmp al, 9
        je .bdone
        cmp al, 13
        je .bdone
        cmp al, 10
        je .bdone
        mov byte [di], ' '
        inc di
        inc bp
        dec cx
        jnz .blank
.bdone: pop ax
        jmp .find
.next:  inc di
        dec cx
        jmp .find
.write: test bp, bp
        jz .cfgdone
        mov bx, [hin]
        mov ax, 0x4200                  ; back to the start
        xor cx, cx
        xor dx, dx
        int 0x21
        mov ah, 0x40
        mov cx, [len]
        mov dx, buf
        int 0x21
        jc ioerr
        mov dx, msg_cfg
        call say
.cfgdone: mov bx, [hin]
        mov ah, 0x3E
        int 0x21
.nocfg: mov dx, msg_done
        call say
        mov ax, 0x4C00
        int 0x21

nokex:  mov dx, msg_nokex
        jmp fail
ioerr:  mov dx, msg_io
        jmp fail
usage:  mov dx, msg_usage
fail:   call say
        mov ax, 0x4C01
        int 0x21

say:    mov ah, 9
        int 0x21
        ret

kexf    db "C:\WINDOWS\SYSTEM\VMM32.KEX", 0
vxdf    db "C:\WINDOWS\SYSTEM\VMM32.VXD", 0
cfgf    db "C:\CONFIG.SYS", 0
msg_ask db "  Windows on that drive was patched to use more than 1 GB of RAM. If it no", 13, 10
        db "  longer starts since then, press U to undo the patch; any other key goes on. $"
msg_crlf db 13, 10, "$"
msg_usage db "usage: KEXUNDO d: [/ASK]   (the drive of the Windows to repair)", 13, 10, "$"
msg_nokex db "No saved original (WINDOWS\SYSTEM\VMM32.KEX) on that drive: nothing to undo.", 13, 10, "$"
msg_io  db "KEXUNDO: disk error; nothing more was changed.", 13, 10, "$"
msg_vmm db "The original VMM32.VXD is back.", 13, 10, "$"
msg_cfg db "CONFIG.SYS: KEXMEM limits Windows to 1 GB again.", 13, 10, "$"
msg_done db "Done. Take out the stick and restart.", 13, 10, "$"
hin     dw 0
hout    dw 0
len     dw 0
buf:
