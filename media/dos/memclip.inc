; KernelExEx - memclip.inc: the int 15h memory-size filter shared by KEXMEM.SYS
; (kexmem.asm) and the boot-sector stage (memstage.asm). Copyright (C) 2026,
; KernelExEx contributors. GPLv2.
; The including file defines, in the same segment as this code (CS-relative):
;   old15 (dd, the previous vector), limit (dd, first byte not reported as RAM),
;   e801bx (dw, the largest E801h/E881h count of 64 KB blocks above 16 MB).
; E820h: usable ranges above the limit become reserved, ranges across it are cut;
; E801h/E881h/88h: the counts are capped. The caller's flags are kept (only CF
; changes), so interrupts stay as the caller had them.
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

