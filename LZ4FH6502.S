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
         jmp   fail

goodmagic
         inc   srcptr
         bne   :nohi
         inc   srcptr+1
:nohi

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
         bne   :nohi
         inc   srcptr+1
:nohi
         lda   (srcptr),y ;get length extension
         clc
         adc   #15        ;add 15 - will not exceed 255

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
shortlit sta   savlen
         tay
:litloop
         lda   (srcptr),y ;5
         dey              ;2  if len is 255, copy 0-254
         sta   (dstptr),y ;6
         bne   :litloop   ;3 -> 16 cycles/byte

* Advance srcptr by savlen+1, and dstptr by savlen
         lda   srcptr
         sec              ;this gets us the +1
         adc   savlen
         sta   srcptr
         bcc   :nohi
         inc   srcptr+1
         clc
:nohi
         lda   dstptr
         adc   savlen
         sta   dstptr
         bcc   :nohi1
         inc   dstptr+1
:nohi1
         ldy   #$ff       ;next INY rolls back to zero

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
         tya
         sec
         adc   srcptr
         sta   srcptr
         bcc   :nohi
         inc   srcptr+1
:nohi

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
         clc
         adc   savlen
         sta   dstptr
         bcc   :nohi1
         inc   dstptr+1
:nohi1

         DO    overrun_check
         LDA   dstptr+1
         CMP   #$60
         BLT   :OKAY
         BRK
         BRK
:OKAY
         jmp   mainloop

         ELSE

         bne   mainloop   ;branch always

         FIN

* handle "special" match values (value in A)
specialmatch
         cmp   #tok_empty
         bne   :notempty
         tya              ;empty match, advance srcptr
         adc   srcptr     ; past and jump to main loop
         sta   srcptr
         bcc   :nohi
         inc   srcptr+1
         clc
:nohi
         jmp   mainloop

:notempty
         cmp   #tok_eod
         bne   fail
         rts              ;success!
fail
         jsr   bell
         jmp   monitor

