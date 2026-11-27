; Soft-scroll pacing test (M-Pacing1), acme syntax:  acme -f cbm -o softscroll.prg softscroll.asm
;
; Every C64 frame (raster busy-wait, no interrupts):
;   - 8 sprites move right by 1..4 pixels (sprite n: (n & 3) + 1), each in its own row band:
;     any repeated or skipped host frame shows as a visible jerk
;   - text row 24 scrolls 1 pixel per frame (hardware XSCROLL via a raster split)
;   - 16 bit frame counter at $fb/$fc (little endian), shown as hex in row 0, column 0
;
; Runs on PAL and NTSC; starts with RUN (BASIC line 10 SYS 2061).

        * = $0801
        !word basicEnd, 10
        !byte $9e
        !text "2061"
        !byte 0
basicEnd
        !word 0

SCREEN  = $0400
COUNTER = $fb           ; frame counter lo/hi
XSCR    = $fd           ; current fine scroll 0..7 (counts down)
TEXTPOS = $fe           ; index of the next scroll text character

start   sei
        lda #$7f        ; CIA interrupts off, KERNAL stays out of the way
        sta $dc0d
        sta $dd0d
        lda $dc0d
        lda $dd0d
        lda #0
        sta $d01a       ; no VIC interrupts
        sta $d020
        sta $d021
        sta COUNTER
        sta COUNTER+1
        sta TEXTPOS
        lda #7
        sta XSCR

        ; screen: spaces, light grey; row 24 white
        ldx #0
-       lda #$20
        sta SCREEN,x
        sta SCREEN+$100,x
        sta SCREEN+$200,x
        sta SCREEN+$2e8,x
        lda #15
        sta $d800,x
        sta $d900,x
        sta $da00,x
        sta $dae8,x
        inx
        bne -
        ldx #39
        lda #1
-       sta $d800+24*40,x
        dex
        bpl -

        ; sprite shape at $0340 (block 13): solid 24x21
        ldx #62
        lda #$ff
-       sta $0340,x
        dex
        bpl -
        ldx #7
-       lda #13
        sta SCREEN+$3f8,x
        lda spriteColor,x
        sta $d027,x
        lda #0
        sta posLo,x
        sta posHi,x
        dex
        bpl -
        ldx #0
        ldy #0
-       lda spriteY,y
        sta $d001,x
        inx
        inx
        iny
        cpy #8
        bne -
        lda #$ff
        sta $d015       ; all sprites on
        lda #0
        sta $d017
        sta $d01d
        sta $d01c
        sta $d01b

        jsr fillRow     ; initial text row

; ------------------------------------------------------------ frame loop
frame
        ; row 24 starts at raster line 51 + 24 * 8 = 243: set its fine scroll a few lines before
        lda #238
        jsr waitLine
        lda XSCR
        ora #$08        ; 40 columns, no multicolor
        sta $d016

        ; below the text row (bottom border): restore and update everything
        lda #252
        jsr waitLine
        lda #$08        ; 40 columns, no scroll for the rest of the screen
        sta $d016

        jsr moveSprites
        jsr scroll
        jsr showCounter

        inc COUNTER
        bne +
        inc COUNTER+1
+       jmp frame

; wait until raster line A (0..255, raster bit 8 clear)
waitLine
-       cmp $d012
        bne -
        bit $d011
        bmi -
        rts

; ------------------------------------------------------------ sprites
moveSprites
        ldx #7
--      txa
        and #3
        clc
        adc #1          ; speed 1..4
        clc
        adc posLo,x
        sta posLo,x
        lda posHi,x
        adc #0
        sta posHi,x
        ; wrap at 344 ($158)
        cmp #1
        bne +
        lda posLo,x
        cmp #$58
        bcc +
        sbc #$58
        sta posLo,x
        lda #0
        sta posHi,x
+       dex
        bpl --

        ; write X positions, MSBs into $d010
        lda #0
        sta msb
        ldx #7
        ldy #14
--      lda posLo,x
        sta $d000,y
        lda posHi,x
        lsr
        rol msb
        dey
        dey
        dex
        bpl --
        lda msb
        sta $d010
        rts

; ------------------------------------------------------------ text scroll (row 24)
scroll  dec XSCR
        bpl ++
        lda #7
        sta XSCR
        ; coarse step: shift the row left by one character, append the next one
        ldx #0
-       lda SCREEN+24*40+1,x
        sta SCREEN+24*40,x
        inx
        cpx #39
        bne -
        jsr nextChar
        sta SCREEN+24*40+39
++      rts

nextChar
        ldx TEXTPOS
        lda message,x
        bne +
        ldx #0
        stx TEXTPOS
        lda message
+       inc TEXTPOS
        rts

fillRow ldx #0
-       stx tmp
        jsr nextChar
        ldx tmp
        sta SCREEN+24*40,x
        inx
        cpx #40
        bne -
        rts

; ------------------------------------------------------------ counter in row 0 (hex, 4 digits)
showCounter
        lda COUNTER+1
        jsr hexHi
        sta SCREEN
        lda COUNTER+1
        jsr hexLo
        sta SCREEN+1
        lda COUNTER
        jsr hexHi
        sta SCREEN+2
        lda COUNTER
        jsr hexLo
        sta SCREEN+3
        rts

hexHi   lsr
        lsr
        lsr
        lsr
hexLo   and #$0f
        tax
        lda hexDigits,x
        rts

; ------------------------------------------------------------ data
hexDigits
        !scr "0123456789abcdef"
spriteColor
        !byte 1, 7, 13, 3, 14, 10, 5, 15
spriteY
        !byte 58, 82, 106, 130, 154, 178, 202, 226
message
        !scr "badline pacing test - every sprite and this text move evenly, any jerk is a pacing error ... "
        !byte 0
msb     !byte 0
tmp     !byte 0
posLo   !fill 8, 0
posHi   !fill 8, 0
