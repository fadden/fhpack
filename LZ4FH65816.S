*********************************
*                               *
* LZ4FH uncompression for 65816 *
* By Andy McFadden              *
* Version 1.0.1, August 2015    *
*                               *
* Refactored for size & speed   *
* by Peter Ferrie.              *
*                               *
* Developed with Merlin-16      *
*                               *
*********************************
         lst   off
         org   $0300

         xc               ;allow 65c02 opcodes
         xc               ;allow 65816 opcodes

*
* Constants
*
lz4fh_magic equ $66       ;ascii 'f'
tok_empty equ  253
tok_eod  equ   254

*
* Variable storage
*
savmix   equ   $00        ;2b
savlen   equ   $02        ;2b

*
* ROM routines
*
bell     equ   $ff3a
monitor  equ   $ff69

*
* Parameters.
*
* in_dst must be $2000 or $4000
*
in_src   equ   $2fc       ;2b
in_dst   equ   $2fe       ;2b

* Main entry point.
entry
         clc              ;go native
         xce
         rep   #$30       ;16-bit acc/index
         mx    %00        ; tell Merlin

         ldx   in_src
         ldy   in_dst
         sty   _dstmod+1

         lda   $0000,x
         inx
         and   #$00ff
         cmp   #lz4fh_magic
         beq   mainloop

fail
         jsr   bell
         jmp   monitor

notempty
         cmp   #tok_eod   ;end-of-data or error

* exit
         sec              ;return to emulation mode
         xce
         bne   fail
         rts

         mx    %00        ;undo the sec/xce

* handle "special" match length values (in A)
specialmatch
         cmp   #tok_empty
         bne   notempty

mainloop
         lda   $0000,x
         inx
         sta   savmix
         and   #$00f0
         beq   noliteral
         lsr   A
         lsr   A
         lsr   A
         lsr   A
         cmp   #$000f
         bne   shortlit

         lda   $0000,x    ;length >= 15, get next
         inx
         and   #$00ff
         adc   #14        ;(carry set) +15 - won't exceed 255

* At this point, Y holds the address of the next
* compressed data byte, X has the address of the
* next output position, and A has the length of
* the literal.
*
* The MVN instruction moves (A+1) bytes from X
* to Y, advancing X and Y.
shortlit
         dec   A          ;MVN wants length-1
         mvn   $00,$00    ;7 cycles/byte

* Now handle the match.
noliteral
         lda   savmix
         and   #$000f
         cmp   #$000f
         blt   :shortmatch ;BCC

         lda   $0000,x    ;add length extension
         inx
         and   #$00ff
         cmp   #237       ;"normal" values are 0-236
         bge   specialmatch
         adc   #15        ;carry clear; won't exceed 255
:shortmatch
         adc   #3         ;min match, -1 for MVN
         sta   savlen     ;spill A while we get offset

         lda   $0000,x    ;load source buffer offset
         inx
         inx
         phx              ;save srcptr for later
_dstmod  ora   #$ff00     ;OR in hi-res page
         tax
         lda   savlen
         mvn   $00,$00
         plx              ;restore srcptr
         bra   mainloop

         lst   on
         sav   LZ4FH65816
         lst   off
