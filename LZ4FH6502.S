********************************
*                              *
* LZ4FH uncompression for 6502 *
* By Andy McFadden             *
* Version 1.0, August 2015     *
*                              *
* Developed with Merlin-16     *
*                              *
********************************
         lst   off
         org   $0300

*
* Constants
*
lz4fh_magic equ $66       ;ascii 'f'
tok_empty equ  253
tok_eod  equ   254

overrun_check equ 0
smaller equ 0

*
* Variable storage
*
srcptr   equ   $3c        ;2b a1l
dstptr   equ   $3e        ;2b a1h
copyptr  equ   $00        ;2b
savmix   equ   $02        ;1b
savlen   equ   $03        ;1b

*
* ROM routines
*
bell     equ   $ff3a
monitor  equ   $ff69

*
* Parameters, stashed at the top of the text input
* buffer.  We use this, rather than just having them
* poked directly into the code, so that the 6502 and
* 65816 implementations work the same way without
* either getting weird.
*
in_src   equ   $2fc       ;2b
in_dst   equ   $2fe       ;2b

entry
         lda   in_src     ;copy source address to zero page
         sta   srcptr
         lda   in_src+1
         sta   srcptr+1
         lda   in_dst     ;copy destination address to zero page
         sta   dstptr
         lda   in_dst+1
         sta   dstptr+1
         sta   _desthi+1

         ldy   #$00
         lda   (srcptr),y
         cmp   #lz4fh_magic ;does magic match?
         beq   goodmagic

fail
         jsr   bell
         jmp   monitor

hi2
         inc   srcptr+1
         bne   nohi2

hi3
         inc   srcptr+1
         clc
         bcc   nohi3

hi4
         inc   dstptr+1
         bne   nohi4

notempty
         cmp   #tok_eod
         bne   fail
         rts              ;success!

;* handle "special" match values (value in A)
specialmatch
         cmp   #tok_empty
         bne   notempty
         DO smaller
advsrc1
         sec
advsrc2
         FIN

         tya              ;empty match, advance srcptr
         adc   srcptr     ; past and jump to main loop
         sta   srcptr
         bcc   mainloop
         inc   srcptr+1
         bne   mainloop

         DO   smaller
         ELSE

hi5
         inc   srcptr+1
         clc
         bcc   nohi5

         FIN

goodmagic
         inc   srcptr
         bne   mainloop
         inc   srcptr+1

mainloop
* Get the mixed-length byte and handle the literal.
         ldy   #$00
         lda   (srcptr),y ;get mixed-length byte
         sta   savmix
         lsr   A          ;get the literal length
         lsr   A
         lsr   A
         lsr   A
         beq   noliteral
         cmp   #$0f
         bne   shortlit

         inc   srcptr
         beq   hi2
nohi2
         lda   (srcptr),y ;get length extension
         adc   #14        ;(carry set) add 15 - will not exceed 255

* At this point, srcptr holds the address of the "mix"
* word or the length extension, and dstptr holds the
* address of the next output location.  So we want to
* read from (srcptr),y+1 and write to (dstptr),y.
* We can do this by sticking the DEY between the LDA
* and STA.
*
* We could save a couple of cycles by substituting
* addr,y in place of (dp),y, but the added setup cost
* would only benefit longer literal strings.
shortlit tax
         tay
:litloop
         lda   (srcptr),y ;5
         dey              ;2  if len is 255, copy 0-254
         sta   (dstptr),y ;6
         bne   :litloop   ;3 -> 16 cycles/byte

* Advance srcptr by savlen+1, and dstptr by savlen
         txa
         sec              ;this gets us the +1
         adc   srcptr
         sta   srcptr
         bcs   hi3
nohi3
         txa
         adc   dstptr
         sta   dstptr
         bcs   hi4
nohi4
         dey              ;next INY rolls back to zero

* Handle match.  Y holds an offset into srcptr such
* that we need to increment it once to get the next
* interesting byte.
noliteral
         lda   savmix
         and   #$0f
         cmp   #$0f
         blt   :shortmatch ;BCC

         iny
         lda   (srcptr),y ;get length extension
         cmp   #237       ;"normal" values are 0-236
         bge   specialmatch ;BCS
         adc   #15        ;will not exceed 255

* Put the destination address into copyptr.
:shortmatch
         adc   #4         ;min match; won't exceed 255
         sta   savlen     ;save match len for later
         tax              ;and keep it in X
         iny
         lda   (srcptr),y ;match offset, lo
         sta   copyptr
         iny
         lda   (srcptr),y ;match offset, hi
_desthi  ora   #$00       ;OR in hi-res page
         sta   copyptr+1

* Advance srcptr past the encoded match while we still
* remember how many bytes it took to encode.  Y is
* indexing the last value used, so we want to go
* advance srcptr by Y+1.

         DO    smaller
         sty   savsrc

         ELSE

         tya
         sec
         adc   srcptr
         sta   srcptr
         bcs   hi5
nohi5
         FIN

* Copy the match.  The length is in X.  Note this
* must be a forward copy so overlapped data works.
*
* We know the match is at least 4 bytes long, so
* we could save a few cycles by not doing the
* ADC #4 earlier, and unrolling the first 4
* load/store operations here.
         ldy   #$00
:copyloop
         lda   (copyptr),y ;5
         sta   (dstptr),y ;6
         iny              ;2
         dex              ;2
         bne   :copyloop  ;3 -> 18 cycles/byte

* advance dstptr past copied data
         lda   dstptr
         adc   savlen
         sta   dstptr

         DO    smaller
         ldy   savsrc
linkadv
         bcc   advsrc

         ELSE

         bcc   mainloop

         FIN

         inc   dstptr+1
         DO    overrun_check
         LDA   dstptr+1
         CMP   #$60

         DO   smaller
         bcc   linkadv

         ELSE

         bcc   mainloop

         FIN

         BRK
         BRK

         ELSE

         DO    smaller
         bne   advsrc2

         ELSE

         bne   mainloop   ;branch always

         FIN

         FIN
