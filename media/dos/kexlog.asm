; KernelExEx - KEXLOG.COM: shows the end of a text file (about the last 20 lines), for
; Windows' start-up log on the install stick: KEXLOG D:\BOOTLOG.TXT
; Copyright (C) 2026, KernelExEx contributors. GPLv2. Build: nasm -f bin -o KEXLOG.COM
        bits 16
        org 0x100
TAIL    equ 640                 ; bytes shown at most (about 20 lines)

start:  cld
        mov si, 0x81            ; the command line: skip blanks, end the name at CR/blank
.skip:  lodsb
        cmp al, ' '
        je .skip
        cmp al, 9
        je .skip
        cmp al, 13
        je usage
        lea dx, [si - 1]
.name:  lodsb
        cmp al, ' '
        je .end
        cmp al, 9
        je .end
        cmp al, 13
        jne .name
.end:   mov byte [si - 1], 0
        mov ax, 0x3D00          ; open, read only
        int 0x21
        jc nofile
        mov bx, ax
        xor cx, cx              ; size: seek to the end
        xor dx, dx
        mov ax, 0x4202
        int 0x21
        jc nofile
        ; dx:ax = size; start at size - TAIL (or 0)
        sub ax, TAIL
        sbb dx, 0
        jnc .seek
        xor ax, ax
        xor dx, dx
.seek:  mov cx, dx
        mov dx, ax
        mov ax, 0x4200
        int 0x21
        mov ah, 0x3F
        mov cx, TAIL
        mov dx, buf
        int 0x21
        jc nofile
        mov cx, ax              ; bytes read
        mov ah, 0x3E
        int 0x21
        jcxz done
        ; begin after the first line break, unless the whole file was read
        mov di, buf
        cmp cx, TAIL
        jb .print
        mov al, 10
        push cx
        repne scasb
        mov ax, di
        pop cx
        jne .print0             ; no line break at all: show everything
        sub ax, buf
        sub cx, ax
        jmp .print
.print0: mov di, buf
.print: mov dx, di
        mov bx, 1               ; stdout
        mov ah, 0x40
        int 0x21
        mov dx, crlf
        mov cx, 2
        mov ah, 0x40
        int 0x21
done:   mov ax, 0x4C00
        int 0x21

nofile: mov dx, msg_nofile
        jmp say
usage:  mov dx, msg_usage
say:    mov ah, 9
        int 0x21
        mov ax, 0x4C01
        int 0x21

msg_usage  db "usage: KEXLOG file", 13, 10, "$"
msg_nofile db "KEXLOG: cannot read the file", 13, 10, "$"
crlf       db 13, 10
buf:
