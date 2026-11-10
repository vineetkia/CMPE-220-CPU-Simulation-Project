; factorial.asm
; Program 1: works out 8! = 40320.
; Written straight in SPARTAN-16 assembly, no compiler involved.
;
; Idea in C:   result = 1;  do { result = result * n;  n = n - 1; } while (n != 0);
;
; R1 holds the result, R2 holds n.
; We also store n and result back to memory every time around the loop,
; so you can watch them change in the memory dump.

start:  LD   R2, n          ; R2 = n (8)
        LI   R1, 1          ; R1 = 1
loop:   MUL  R1, R1, R2     ; result = result * n
        ST   R1, result     ; save result in memory
        ADDI R2, R2, -1     ; n = n - 1 (Z flag becomes 1 when n hits 0)
        ST   R2, n          ; save n in memory
        BNE  loop           ; go back if n is not 0 yet
        OUT  R1             ; print the answer
        HALT

n:      .word 8             ; input
result: .word 0             ; answer ends up here
