; The program which test_symbols.py runs from AmigaDOS, built with vasm and
; vlink. Once per frame it adds 1 to counter, copies JOY1DAT to joystick and
; copies colour to COLOR00.

        section code,code

start:
        lea     $dff000,a5
        move.w  #$7fff,$9a(a5)          ; INTENA: interrupts off
        move.w  #$7fff,$96(a5)          ; DMACON: DMA off
        lea     counter,a0
        moveq   #0,d0
wait:
        move.w  $1e(a5),d1              ; INTREQR
        btst    #5,d1                   ; vertical blank
        beq.s   wait
        move.w  #$0020,$9c(a5)          ; INTREQ: clear the bit
        bsr.s   update
        bra.s   wait

update:
        addq.l  #1,d0
        move.l  d0,(a0)
; C compilers put an underscore before the names in the source code.
_store_input:
        move.w  $c(a5),joystick
        move.w  colour,$180(a5)
        rts

        section data,data

counter:
        dc.l    0
joystick:
        dc.w    0
colour:
        dc.w    $0f00
