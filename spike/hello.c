/*
 * Spike: DWARF-in-PE control test.
 *
 * Compiled with:
 *   gcc -O0 -gdwarf-5 -o hello.exe hello.c
 *
 * Goal: produce a Windows PE with embedded DWARF v5 and verify that
 *   gdb (and VSCode's cppdbg adapter) can step it. If THIS doesn't
 *   work, our plan to inject DWARF into Delphi PEs is doomed and we
 *   pivot to a sidecar symbol-file approach.
 */

#include <stdio.h>

static int add(int a, int b) {
    int sum = a + b;
    return sum;
}

int main(void) {
    int x = 3;
    int y = 4;
    int result = add(x, y);
    printf("result = %d\n", result);
    return 0;
}
