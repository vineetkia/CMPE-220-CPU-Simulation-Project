/*
 * multiply.c
 * Program 2: multiply two numbers using only addition (5 x 3 = 15).
 * We add a to result, b times. No multiply instruction is used.
 *
 * This is normal C, so gcc can build it too. For SPARTAN-16 we translated it
 * into our assembly and then into binary by hand. The hand translation is in
 * src/programs.c.
 */
#include <stdio.h>

int a = 5;
int b = 3;
int result = 0;

int main(void)
{
    while (b > 0) {
        result = result + a;
        b = b - 1;
    }
    printf("%d\n", result);
    return 0;
}
