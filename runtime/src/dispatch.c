/* dispatch.c - map (bank, address) to recompiled C functions, with the
 * interpreter as fallback for anything the recompiler did not cover. */
#include "gg.h"
#include <stdio.h>
#include "z80ops.h"

typedef struct { uint16_t bank, addr; gg_fn fn; } gg_entry;
extern const gg_entry gg_recomp_table[];
extern const unsigned gg_recomp_count;

gg_fn gg_lookup(unsigned bank, uint16_t addr)
{
    unsigned lo = 0, hi = gg_recomp_count;
    uint32_t key = (bank << 16) | addr;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        uint32_t k = ((uint32_t)gg_recomp_table[mid].bank << 16) | gg_recomp_table[mid].addr;
        if (k == key) return gg_recomp_table[mid].fn;
        if (k < key) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

static inline gg_fn lookup_pc(uint16_t pc)
{
    if (pc >= 0xC000) return NULL;          /* code in RAM: always interpreted */
    return gg_lookup(gg_bank_at(pc), pc);
}

uint64_t gg_interp_steps;                   /* instructions run by the fallback */

/* The game builds an unrolled "OUTI x 256; RET" VRAM blaster at $DD00 (see
 * $02DA) and calls into it at various offsets.  Run it natively with exactly
 * the per-instruction timing/IRQ checks the interpreter would apply. */
static int ram_outi_block(Z80 *z)
{
    uint16_t pc = z->pc;
    if (pc < 0xDD00 || pc > 0xDF00 || (pc & 1)) return 0;
    for (uint16_t a = pc; a < 0xDF00; a += 2)
        if (gg.ram[a & 0x1FFF] != 0xED || gg.ram[(a + 1) & 0x1FFF] != 0xA3) return 0;
    if (gg.ram[0xDF00 & 0x1FFF] != 0xC9) return 0;
    for (; pc < 0xDF00; pc += 2) {
        INC_R(z, 2);
        blk_out(z, 1);
        z->cycles += 16;
        GG_TRACE_HOOK(z, (uint16_t)(pc + 2));
        if (GG_NEED_CHECK(z)) gg_check(z, (uint16_t)(pc + 2));
    }
    INC_R(z, 1);
    z->pc = pop(z);
    z->cycles += 10;
    GG_TRACE_HOOK(z, z->pc);
    if (GG_NEED_CHECK(z)) gg_check(z, z->pc);
    return 1;
}

void gg_log_miss(unsigned bank, uint16_t addr)
{
    /* Each line is a code entry the static tracer missed.  Feed the file back
     * into tools/hints.json and re-run the recompiler. */
    static FILE *log;
    static uint32_t seen[2048];
    uint32_t key = (bank << 16) | addr, h = (key * 2654435761u) >> 21;
    for (int i = 0; i < 2048; i++) {
        uint32_t *s = &seen[(h + i) & 2047];
        if (*s == key + 1) return;
        if (!*s) { *s = key + 1; break; }
    }
    if (!log) log = fopen("recomp_misses.txt", "a");
    if (log) { fprintf(log, "%02X:%04X\n", bank, addr); fflush(log); }
}

void gg_jump(Z80 *z)
{
    gg_fn fn = lookup_pc(z->pc);
    if (fn) { fn(z); return; }
    if (z->pc >= 0xC000 && ram_outi_block(z)) return;
    if (z->pc < 0xC000) gg_log_miss(gg_bank_at(z->pc), z->pc);

    /* Interpret until the code returns past the current stack level, hopping
     * back into recompiled code whenever it reaches a known entry point. */
    uint16_t sp0 = z->sp;
    while ((int16_t)(z->sp - sp0) <= 0) {
        fn = lookup_pc(z->pc);
        if (fn) { fn(z); continue; }
        z80_step(z);
        gg_interp_steps++;
        GG_TRACE_HOOK(z, z->pc);
        if (GG_NEED_CHECK(z)) gg_check(z, z->pc);
    }
}

/* A call returned with an unexpected PC/SP.  `ret_pc` is the address the
 * caller pushed and `sp_before` the SP before that push.
 *   SP below sp_before: the callee is still running (e.g. it pushed an address
 *     and jumped) -> keep executing at PC.
 *   SP == sp_before:  back at the caller's level; if PC == ret_pc it was a
 *     normal return (0: caller continues).  Otherwise the code continues at PC
 *     in place of the caller -> run it, then the caller must return (1).
 *   SP above sp_before: the callee unwound past the caller's frame -> the
 *     caller must return at once and let an outer frame continue (1). */
int gg_resume(Z80 *z, uint16_t ret_pc, uint16_t sp_before)
{
    for (;;) {
        int16_t d = (int16_t)(z->sp - sp_before);
        if (d == 0 && z->pc == ret_pc) return 0;
        if (d < 0) { gg_jump(z); continue; }
        if (d == 0) gg_jump(z);
        return 1;
    }
}

void gg_call(Z80 *z, uint16_t addr)
{
    z->pc = addr;
    gg_jump(z);
}
