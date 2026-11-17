; KernelExEx - mbr.asm: a master boot record. Loads the active partition's
; boot sector (LBA through int 13h extensions, else CHS) to 0:7C00 and runs it
; with DL = the boot drive and DS:SI = its partition entry, as DOS expects.
; Copyright (C) 2026, KernelExEx contributors. GPLv2. Build: nasm -f bin.
        bits 16
        org 0x600
start:
        cli
        xor ax, ax
        mov ss, ax
        mov sp, 0x7C00
        mov ds, ax
        mov es, ax
        sti
        cld
        mov si, 0x7C00          ; move ourselves out of the way to 0:0600
        mov di, 0x600
        mov cx, 256
        rep movsw
        jmp 0:moved
moved:
        mov [drive], dl
        mov si, part
        mov cx, 4
.find:  test byte [si], 0x80
        jnz found
        add si, 16
        loop .find
        mov si, msg_noact
        jmp fail
found:
        mov [entry], si
        ; extensions present?
        mov ah, 0x41
        mov bx, 0x55AA
        mov dl, [drive]
        int 0x13
        jc .chs
        cmp bx, 0xAA55
        jne .chs
        test cl, 1
        jz .chs
        mov si, [entry]
        mov eax, [si + 8]
        mov [dap_lba], eax
        mov si, dap
        mov ah, 0x42
        mov dl, [drive]
        int 0x13
        jnc boot
.chs:   mov si, [entry]
        mov dh, [si + 1]
        mov cx, [si + 2]
        mov bx, 0x7C00
        mov ax, 0x0201
        mov dl, [drive]
        int 0x13
        jc read_err
boot:   cmp word [0x7DFE], 0xAA55
        jne read_err
        mov si, [entry]
        mov dl, [drive]
        jmp 0:0x7C00
read_err: mov si, msg_err
fail:   lodsb
        test al, al
        jz $
        mov ah, 0x0E
        mov bx, 7
        int 0x10
        jmp fail

msg_noact db "No active partition", 0
msg_err db "Disk read error", 0
drive   db 0
entry   dw 0
dap:    db 16, 0
        dw 1
        dw 0x7C00, 0
dap_lba dd 0, 0

        times 0x1B8 - ($ - $$) db 0
disksig dd 0
        dw 0
part:   times 64 db 0
        dw 0xAA55
