/* gcd.c: greatest common divisor using subtraction (our CPU has no divide). */
#include <stdio.h>

int gcd(int a, int b) {
    while (a != b) {
        if (a > b)
            a = a - b;
        else
            b = b - a;
    }
    return a;
}

int main() {
    printf("%d\n", gcd(84, 36));
    printf("%d\n", gcd(17, 5) + gcd(1000, 750) * 2);
    return 0;
}
