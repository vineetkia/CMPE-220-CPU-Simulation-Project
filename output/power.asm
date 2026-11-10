; ============================================================
; power.c compiled to SPARTAN-16 assembly by s16cc
; each C variable has its own memory word, named function.name
; ============================================================
_start:
        LI    SP, stack_top       ; SP points just past the stack
        CALL  main                ; run the C program
        HALT                      ; main is done, stop

; ------------------------------------------------------------
; int power(int base, int exp)
; ------------------------------------------------------------
power:
        ; int result = 1;
        LI    R1, 1
        ST    R1, power.result
        ; while (exp > 0) {
power.while1:
        LD    R1, power.exp
        CMP   R1, R0
        BLE   power.endwhile1
        ; result = result * base;
        LD    R1, power.result
        LD    R2, power.base
        MUL   R1, R1, R2
        ST    R1, power.result
        ; exp = exp - 1;
        LD    R1, power.exp
        ADDI  R1, R1, -1
        ST    R1, power.exp
        JMP   power.while1
power.endwhile1:
        ; return result;
        LD    R1, power.result
        RET

; ------------------------------------------------------------
; int main()
; ------------------------------------------------------------
main:
        ; int x = power(8, 2);
        LI    R1, 8
        LI    R2, 2
        ST    R1, power.base      ; argument base
        ST    R2, power.exp       ; argument exp
        CALL  power
        ST    R1, main.x
        ; printf("%d\n", x);
        LD    R1, main.x
        OUT   R1
        ; return 0;
        LI    R1, 0
        RET

; ------------------------------------------------------------
; data: one memory word for each C variable
; ------------------------------------------------------------
power.base:      .word 0
power.exp:       .word 0
power.result:    .word 0
main.x:          .word 0

; stack (8 words, it grows down from stack_top)
stack:           .space 8
stack_top:
