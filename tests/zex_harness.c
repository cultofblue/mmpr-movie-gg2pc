/* zex_harness.c - runs zexdoc/zexall (CP/M .COM) on the interpreter core.
 * Build: cc -O2 -I../runtime/include -o zex zex_harness.c
 * Usage: ./zex zexdoc.com */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static uint8_t mem[65536];
#define Z80_RD(a)     mem[(uint16_t)(a)]
#define Z80_WR(a, v)  (mem[(uint16_t)(a)] = (v))
#define Z80_IN(p)     0xFF
#define Z80_OUT(p, v) ((void)(p), (void)(v))
#include "../runtime/src/interp.c"

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fread(mem + 0x100, 1, 0xFF00, f);
    fclose(f);
    z80_init();
    Z80 z = {0};
    z.pc = 0x100; z.sp = 0xF000;
    mem[0] = 0x76;                   /* warm boot -> stop */
    mem[5] = 0xC9;                   /* BDOS: trapped below, then RET */
    mem[6] = 0x00; mem[7] = 0xF0;    /* top of TPA */
    for (;;) {
        if (z.pc == 0x0000) break;
        if (z.pc == 0x0005) {
            if (z.c == 2) putchar(z.e);
            else if (z.c == 9) for (uint16_t a = z.de; mem[a] != '$'; a++) putchar(mem[a]);
            fflush(stdout);
        }
        z80_step(&z);
    }
    printf("\n[cycles %llu]\n", (unsigned long long)z.cycles);
    return 0;
}
