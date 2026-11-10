/* factorial.c: 8! = 40320 written in C, using a for loop. */
#include <stdio.h>

int factorial(int n) {
    int result = 1;
    for (int i = 2; i <= n; i++) {
        result = result * i;
    }
    return result;
}

int main() {
    printf("%d\n", factorial(8));
    return 0;
}
