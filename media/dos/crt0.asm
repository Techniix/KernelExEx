; KernelExEx - crt0.asm: start-up and 32-bit arithmetic helpers for our small
; DOS programs (Open Watcom C, small model, no C library). GPLv2.
; Build: wasm -zq -ms crt0.asm
        .386
        .model small

        public  _cstart_
        public  __psp
        public  __U4D
        public  __I4D
        public  __U4M
        public  __I4M
        extrn   main_ : near

        .data
__psp   dw      0

_BSS    segment word public 'BSS'
__bss_start label byte
_BSS    ends

STACK   segment para stack 'STACK'
__bss_end label byte
        db      4096 dup (?)
__stack_top label byte
STACK   ends

DGROUP  group   _DATA, _BSS, STACK

        .code
_cstart_:
        mov     ax, DGROUP
        mov     ds, ax
        mov     __psp, es
        ; small model: SS must be DGROUP too (the loader set it to STACK's own frame)
        cli
        mov     ss, ax
        mov     sp, offset DGROUP:__stack_top
        sti
        ; the loader does not clear uninitialised data: do it
        push    es
        mov     es, ax
        mov     di, offset DGROUP:__bss_start
        mov     cx, offset DGROUP:__bss_end
        sub     cx, di
        xor     al, al
        cld
        rep     stosb
        pop     es
        ; give back the memory above DGROUP's end (DOS gave us all of it)
        mov     bx, offset DGROUP:__stack_top
        shr     bx, 4
        inc     bx
        mov     ax, ds
        add     bx, ax
        mov     ax, es
        sub     bx, ax
        mov     ah, 4Ah
        int     21h
        push    ds
        pop     es
        call    main_
        mov     ah, 4Ch
        int     21h

; unsigned long divide: DX:AX / CX:BX -> quotient DX:AX, remainder CX:BX
__U4D:
        shl     eax, 16
        shrd    eax, edx, 16            ; eax = DX:AX
        shl     ecx, 16
        mov     cx, bx                  ; ecx = CX:BX
        xor     edx, edx
        div     ecx
        mov     ebx, edx
        mov     ecx, edx
        shr     ecx, 16                 ; CX:BX = remainder
        mov     edx, eax
        shr     edx, 16                 ; DX:AX = quotient
        ret

; signed long divide: same registers
__I4D:
        push    esi
        shl     eax, 16
        shrd    eax, edx, 16
        shl     ecx, 16
        mov     cx, bx
        mov     esi, ecx
        cdq
        idiv    esi
        mov     ebx, edx
        mov     ecx, edx
        shr     ecx, 16
        mov     edx, eax
        shr     edx, 16
        pop     esi
        ret

; long multiply: DX:AX * CX:BX -> DX:AX
__U4M:
__I4M:
        shl     eax, 16
        shrd    eax, edx, 16
        mov     edx, ecx
        shl     edx, 16
        mov     dx, bx
        imul    eax, edx
        mov     edx, eax
        shr     edx, 16
        ret

        end     _cstart_
