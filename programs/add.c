/*
 * add.c
 * Program 1: add two numbers (5 + 7 = 12).
 *
 * This is normal C, so gcc can build it too. For SPARTAN-16 we translated it
 * into our assembly and then into binary by hand. The hand translation is in
 * src/programs.c.
 */
#include <stdio.h>

int a = 5;
int b = 7;
int c;

int main(void)
{
    c = a + b;
    printf("%d\n", c);
    return 0;
}
