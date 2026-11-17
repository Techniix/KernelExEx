; KEXMEM.SYS - KernelExEx memory limiter and CPU state cleaner for Windows 98/ME
; Copyright (C) 2026 KernelExEx contributors. GPLv2 (see COPYING).
;
; Load it as the FIRST line of CONFIG.SYS:
;     DEVICE=C:\KEXMEM.SYS [/MAX=n] [/NOCR] [/Q]
;
; 1. Memory limit. Unpatched Windows 98/98 SE/ME cannot run with much more than
;    1 GB of RAM: VMM and VCACHE run out of the 1 GB system arena and Windows
;    crashes during start-up. KEXMEM hooks the BIOS memory-size services (int 15h
;    E820h, E801h, E881h, 88h) so that DOS, HIMEM.SYS and Windows all see RAM only
;    below n MB (default 1024, range 64..3072). Memory above the limit is reported
;    as reserved; nothing in Windows is patched.
; 2. Control registers. Some CSM / UEFI compatibility BIOSes hand over with CR0,
;    CR4 or EFER bits still set ("While initializing device VCACHE: Windows
;    protection error"). KEXMEM resets them to their power-on values, checking
;    with CPUID which registers exist. The idea comes from mintsuki's CREGFIX
;    (0BSD); this is an independent implementation. /NOCR skips this step.
;
; Build: nasm -f bin -o KEXMEM.SYS kexmem.asm

bits 16
org 0

hdr:    dd -1
        dw 0x8000                       ; character device
        dw strategy
        dw interrupt
        db 'KEXMEM$ '

req:    dd 0
old15:  dd 0
limit:  dd 0x40000000                   ; first byte NOT reported as RAM
e801bx: dw 0                            ; max 64 KB blocks above 16 MB

; ---------------------------------------------------------------- resident part
int15:
        cmp ax, 0xE820
        je .e820
        cmp ax, 0xE801
        je .e801
        cmp ax, 0xE881
        je .e881
        cmp ah, 0x88
        je .f88
        jmp far [cs:old15]

.e820:  pushf
        call far [cs:old15]             ; returns with its own flags
        jc .ret                         ; error: pass straight back
        push eax
        push ebx
        push ecx
        push edx
        ; entry at ES:DI: base(8) length(8) type(4)
        cmp dword [es:di+16], 1         ; only usable RAM is limited
        jne .e820x
        mov eax, [es:di]                ; base low
        mov edx, [es:di+4]              ; base high
        test edx, edx
        jnz .e820r                      ; above 4 GB: reserved
        cmp eax, [cs:limit]
        jae .e820r                      ; starts above the limit
        ; end = base + length (64-bit); clip if above the limit
        mov ebx, [es:di+8]
        mov ecx, [es:di+12]
        add ebx, eax
        adc ecx, 0
        jnz .e820c
        cmp ebx, [cs:limit]
        jbe .e820x
.e820c: mov ebx, [cs:limit]
        sub ebx, eax
        mov [es:di+8], ebx
        mov dword [es:di+12], 0
        jmp .e820x
.e820r: mov dword [es:di+16], 2         ; reserved
.e820x: pop edx
        pop ecx
        pop ebx
        pop eax
        clc
; Return to the caller with ITS flags (interrupt flag included), changing only
; CF to the current one. A plain RETF 2 would return with IF=0 (INT clears it).
.ret:   push bp
        mov bp, sp
        jc .setc
        and byte [bp+6], 0xFE
        pop bp
        iret
.setc:  or byte [bp+6], 1
        pop bp
        iret

.e801:  pushf
        call far [cs:old15]
        jc .ret
        cmp bx, [cs:e801bx]
        jbe .e801a
        mov bx, [cs:e801bx]
.e801a: cmp dx, [cs:e801bx]
        jbe .ok
        mov dx, [cs:e801bx]
.ok:    clc
        jmp .ret

.e881:  pushf
        call far [cs:old15]
        jc .ret
        push eax
        movzx eax, word [cs:e801bx]
        cmp ebx, eax
        jbe .e881a
        mov ebx, eax
.e881a: cmp edx, eax
        jbe .e881b
        mov edx, eax
.e881b: pop eax
        clc
        jmp .ret

.f88:   pushf
        call far [cs:old15]
        jc .ret
        push bx                         ; KB above 1 MB, at most limit-1 MB
        mov bx, [cs:limit+2]            ; limit in 64 KB units (limit < 4 GB)
        cmp bx, 0x0400                  ; >= 64 MB: 16-bit value can't exceed it
        jae .f88x
        shl bx, 6                       ; 64 KB -> KB
        sub bx, 1024
        cmp ax, bx
        jbe .f88x
        mov ax, bx
.f88x:  pop bx
        clc
        jmp .ret

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
        ; command line: far pointer at request+18, text after DEVICE=
        push es
        push bx
        lds si, [es:bx+18]
        call parse                      ; sets cs:mb, cs:nocr, cs:quiet
        push cs
        pop ds
        pop bx
        pop es

        ; this driver uses 386 instructions; refuse on older CPUs
        call is386
        jc .no386

        ; limit = mb << 20 ; E801 blocks = (limit - 16 MB) / 64 KB
        movzx eax, word [mb]
        shl eax, 20
        mov [limit], eax
        sub eax, 0x01000000
        shr eax, 16
        mov [e801bx], ax

        cmp byte [nocr], 0
        jne .hook
        call fixcr

.hook:  xor ax, ax
        push es
        mov es, ax
        cli
        mov eax, [es:0x15*4]
        mov [old15], eax
        mov word [es:0x15*4], int15
        mov [es:0x15*4+2], cs
        sti
        pop es

        cmp byte [quiet], 0
        jne .q
        mov dx, msg_ok1
        call puts
        mov ax, [mb]
        call putdec
        mov dx, msg_ok2
        call puts
        mov dx, msg_cr
        cmp byte [nocr], 0
        je .crm
        mov dx, msg_nocr
.crm:   call puts
.q:     les bx, [req]
        mov word [es:bx+14], resident_end
        mov [es:bx+16], cs
        ret

.no386: mov dx, msg_386
        call puts
        les bx, [req]
        mov word [es:bx+14], 0          ; don't stay resident
        mov [es:bx+16], cs
        ret

; Reset control registers to power-on state (real mode, paging off, caches on).
fixcr:
        smsw ax                         ; already in V86 mode (an EMM loaded
        test al, 1                      ; first)? then CRs can't be touched
        jnz .skip
        pushfd
        cli
        mov eax, cr0
        and eax, 0x00000010             ; keep ET only: paging/cache-off/WP cleared
        mov cr0, eax
        xor eax, eax
        mov cr3, eax
        call has_cpuid
        jc .done
        mov eax, 1
        cpuid
        test edx, 0x0000204E            ; CR4 exists if VME/DE/PSE/PAE/PGE
        jz .efer
        xor eax, eax
        mov cr4, eax
.efer:  mov eax, 0x80000000
        cpuid
        cmp eax, 0x80000001
        jb .done
        mov eax, 0x80000001
        cpuid
        test edx, 0x20100800            ; LM, NX or SYSCALL => EFER exists
        jz .done
        mov ecx, 0xC0000080
        xor eax, eax
        xor edx, edx
        wrmsr
.done:  popfd
.skip:  ret

; CF=1 if not a 386 or later.
is386:  pushf
        pushf
        pop ax
        or ax, 0x7000                   ; try to set IOPL/NT (only sticks on 386+)
        push ax
        popf
        pushf
        pop ax
        popf
        test ax, 0x7000
        jz .no
        clc
        ret
.no:    stc
        ret

; CF=1 if CPUID is missing (EFLAGS.ID can't be toggled).
has_cpuid:
        pushfd
        pop eax
        mov ecx, eax
        xor eax, 0x00200000
        push eax
        popfd
        pushfd
        pop eax
        push ecx
        popfd
        xor eax, ecx
        test eax, 0x00200000
        jz .no
        clc
        ret
.no:    stc
        ret

; DS:SI = command line. Recognises /MAX=n, /NOCR, /Q (case-insensitive).
parse:
.next:  lodsb
        cmp al, 13
        je .end
        cmp al, 10
        je .end
        test al, al
        je .end
        cmp al, '/'
        jne .next
        lodsb
        and al, 0xDF
        cmp al, 'M'
        je .max
        cmp al, 'N'
        je .nocr
        cmp al, 'Q'
        jne .next
        mov byte [cs:quiet], 1
        jmp .next
.nocr:  mov byte [cs:nocr], 1
        jmp .next
.max:   lodsb                           ; skip to the digits after '='
        cmp al, '='
        je .num
        cmp al, ':'
        je .num
        cmp al, 13
        je .end
        test al, al
        je .end
        cmp al, ' '
        je .next
        jmp .max
.num:   xor bx, bx
.dig:   lodsb
        sub al, '0'
        cmp al, 9
        ja .numx
        imul bx, bx, 10
        movzx ax, al
        add bx, ax
        cmp bx, 10000
        jb .dig
.numx:  dec si
        cmp bx, 64
        jb .next                        ; out of range: keep the default
        cmp bx, 3072
        ja .next
        mov [cs:mb], bx
        jmp .next
.end:   ret

puts:   mov ah, 9
        int 0x21
        ret

putdec: mov cx, 0                       ; AX -> decimal
        mov bx, 10
.d1:    xor dx, dx
        div bx
        push dx
        inc cx
        test ax, ax
        jnz .d1
.d2:    pop dx
        add dl, '0'
        mov ah, 2
        int 0x21
        loop .d2
        ret

mb:     dw 1024
nocr:   db 0
quiet:  db 0
msg_ok1:  db 'KEXMEM: memory limited to $'
msg_ok2:  db ' MB for Windows', 13, 10, '$'
msg_cr:   db 'KEXMEM: CPU control registers reset', 13, 10, '$'
msg_nocr: db 'KEXMEM: control registers left as they are (/NOCR)', 13, 10, '$'
msg_386:  db 'KEXMEM: needs a 386 or later, not loaded', 13, 10, '$'
