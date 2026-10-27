/* machine.c - scheduler shared by the interpreter and recompiled code.
 *
 * One frame = 262 scanlines x 228 T-states (59.92 Hz).  Scanline work happens
 * when the CPU's cycle counter crosses a line boundary; the check runs between
 * every pair of instructions in both execution modes, so the two modes produce
 * identical machine state (verified by tests/difftest). */
#include "gg.h"
#include <stdio.h>

#define CYCLES_PER_LINE 228
#define LINES_PER_FRAME 262
#define CYCLES_PER_FRAME (CYCLES_PER_LINE * LINES_PER_FRAME)

static void end_frame(void)
{
    Z80 *cpu = &gg.cpu;
    double want = (double)GG_AUDIO_RATE * CYCLES_PER_FRAME / 3579545.0 + gg.audio_frac;
    int n = (int)want;
    gg.audio_frac = want - n;
    gg.audio_samples = n;
    uint64_t start = gg.frame_start_cycle;
    psg_render(gg.audio, n, GG_AUDIO_RATE, start, CYCLES_PER_FRAME);
    gg.frame++;
    (void)cpu;
    if (gg.frame_callback) gg.frame_callback();
}

void gg_lines(void)
{
    while (gg.cpu.cycles >= gg.next_line_cycle) {
        int line = gg.cur_line;
        if (line == 0) gg.frame_start_cycle = gg.next_line_cycle;
        gg.vdp_line = line;
        vdp_line_tick(line);
        if (line < 192) vdp_render_line(line);
        gg.next_line_cycle += CYCLES_PER_LINE;
        if (++gg.cur_line == LINES_PER_FRAME) {
            gg.cur_line = 0;
            end_frame();
        }
    }
}

void gg_take_irq(Z80 *z, uint16_t next_pc)
{
    z->pc = next_pc;
    z80_irq(z);                     /* pushes next_pc, pc = $0038, +13 cycles */
    if (gg.recomp) {
        /* run the handler as a nested call; it ends with RETI which pops next_pc */
        uint16_t sp_before = (uint16_t)(z->sp + 2);
#ifdef GG_TRACE
        extern int gg_irq_depth; gg_irq_depth++;
#endif
        gg_call(z, 0x0038);
#ifdef GG_TRACE
        gg_irq_depth--;
#endif
        if (z->pc != next_pc || z->sp != sp_before) {
#ifdef GG_TRACE
            fprintf(stderr, "irq ret mismatch: next=%04X pc=%04X sp=%04X want sp=%04X cyc=%llu\n", next_pc, z->pc, z->sp, sp_before, (unsigned long long)z->cycles);
#endif
            if (gg_resume(z, next_pc, sp_before)) {
                /* the handler changed where the interrupted code continues;
                 * that continuation has now run to completion at this level */
                gg_log_miss(0xFE, next_pc);
            }
        }
    }
}

static void check_irq(Z80 *z, uint16_t next_pc)
{
    if (z->ei_delay) { z->ei_delay = 0; return; }
    if (z->iff1 && gg.irq) gg_take_irq(z, next_pc);
}

void gg_check(Z80 *z, uint16_t next_pc)
{
    if (z->cycles >= gg.next_line_cycle) {
        gg.resume_pc = next_pc;
        gg_lines();
    }
    check_irq(z, next_pc);
}

void (*gg_step_hook)(Z80 *cpu);

static void run_interpreter_forever(Z80 *z)
{
    for (;;) {
        if (gg_step_hook) gg_step_hook(z);
        z80_step(z);
        GG_TRACE_HOOK(z, z->pc);
        if (GG_NEED_CHECK(z)) gg_check(z, z->pc);
    }
}

/* Resume a loaded save state.  The state was captured inside gg_lines() at an
 * instruction boundary (resume_pc), but the C call stack that recompiled code
 * had at that moment is gone.  Finish the interrupted check, then interpret
 * from resume_pc, jumping into recompiled functions whenever execution reaches
 * one of their entry points.  Returns always go through the real Z80 stack,
 * so within a few calls nearly everything runs natively again. */
void gg_boot_resume(void)
{
    Z80 *z = &gg.cpu;
    z->pc = gg.resume_pc;
    check_irq(z, z->pc);
    for (;;) {
        if (gg.recomp && z->pc < 0xC000) {
            gg_fn fn = gg_lookup(gg_bank_at(z->pc), z->pc);
            if (fn) { fn(z); continue; }
        }
        z80_step(z);
        GG_TRACE_HOOK(z, z->pc);
        if (GG_NEED_CHECK(z)) gg_check(z, z->pc);
    }
}

void gg_boot(void)
{
    gg.next_line_cycle = 0;
    gg.cur_line = 0;
    gg.cpu.cycles = 0;
    if (gg.recomp) {
        gg.cpu.pc = 0x0000;
        gg_jump(&gg.cpu);          /* reset vector never returns */
        gg_log_miss(0xFF, gg.cpu.pc);
    }
    run_interpreter_forever(&gg.cpu);
}
