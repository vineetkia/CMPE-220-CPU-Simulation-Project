; factorial_of_3.asm
; Small sample program: works out 3! = 3 x 2 x 1 = 6.
; Same loop as factorial.asm, but n = 3 so the whole run is only
; 19 instructions. Good one to walk through by hand.
;
; R1 holds the result, R2 holds n.

start:  LD   R2, n          ; R2 = n (3)
        LI   R1, 1          ; R1 = 1
loop:   MUL  R1, R1, R2     ; result = result * n
        ST   R1, result     ; save result in memory
        ADDI R2, R2, -1     ; n = n - 1 (Z flag becomes 1 when n hits 0)
        ST   R2, n          ; save n in memory
        BNE  loop           ; go back if n is not 0 yet
        OUT  R1             ; print the answer
        HALT

n:      .word 3
result: .word 0
