/* fibonacci.c: prints the first 10 Fibonacci numbers. */
#include <stdio.h>

int main() {
    int a = 0, b = 1;
    for (int i = 0; i < 10; i++) {
        printf("%d\n", a);
        int t = a + b;
        a = b;
        b = t;
    }
    return 0;
}
