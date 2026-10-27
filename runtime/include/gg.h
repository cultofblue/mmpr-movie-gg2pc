/* gg.h - Game Gear hardware runtime shared by recompiled code and the
 * fallback interpreter.  Everything the original Z80 code can observe lives
 * here: CPU registers, memory map (Sega mapper), I/O ports. */
#ifndef GG_H
#define GG_H

#include <stdint.h>
#include <stddef.h>

#define GG_SCREEN_W 160
#define GG_SCREEN_H 144
#define GG_AUDIO_RATE 44100
#define GG_ROM_CRC32 0xB47C19E5u /* MMPR: The Movie (USA, Europe, Brazil) (En) */

/* ---------------- CPU state ---------------- */
typedef struct {
    union { struct { uint8_t f, a; }; uint16_t af; };
    union { struct { uint8_t c, b; }; uint16_t bc; };
    union { struct { uint8_t e, d; }; uint16_t de; };
    union { struct { uint8_t l, h; }; uint16_t hl; };
    uint16_t af_, bc_, de_, hl_;           /* shadow set */
    union { struct { uint8_t ixl, ixh; }; uint16_t ix; };
    union { struct { uint8_t iyl, iyh; }; uint16_t iy; };
    uint16_t sp, pc;
    uint8_t i, r, im, iff1, iff2, halted;
    uint8_t ei_delay;                      /* EI takes effect after next insn */
    uint64_t cycles;
} Z80;

/* ---------------- machine ---------------- */
typedef struct {
    const uint8_t *rom;
    size_t rom_size;
    unsigned rom_banks;
    uint8_t ram[0x2000];          /* $C000-$DFFF, mirrored at $E000-$FFFF */
    uint8_t slot[3];              /* banks mapped in slots 0..2 ($FFFD-$FFFF) */
    uint8_t mapper_ctrl;          /* $FFFC */

    /* VDP */
    uint8_t vram[0x4000];
    uint8_t cram[64];             /* GG: 32 colours x 12-bit */
    uint8_t vdp_reg[16];
    uint16_t vdp_addr;
    uint8_t vdp_code, vdp_latch, vdp_latch_pending, vdp_status, vdp_buffer;
    int vdp_line;
    uint8_t vdp_line_counter;
    uint8_t vdp_irq_line_pending;
    uint8_t vdp_vscroll_latch;
    uint32_t framebuffer[GG_SCREEN_W * GG_SCREEN_H];

    /* PSG */
    uint8_t psg_latch;
    uint16_t psg_tone[3];
    uint8_t psg_vol[4];
    uint8_t psg_noise;
    uint8_t psg_stereo;           /* GG port $06 */
    int32_t psg_counter[4];       /* generator state (kept here so save states capture it) */
    uint8_t psg_output[4];
    uint8_t psg_nflip;
    uint16_t psg_lfsr;
    double psg_tick_acc;

    /* input */
    uint8_t pad;                  /* active-low: U D L R 1 2 in bits 0..5 */
    uint8_t start;                /* 1 = pressed */

    /* audio produced by the last frame (stereo interleaved, GG_AUDIO_RATE) */
    int16_t audio[2 * 1024];
    int audio_samples;
    double audio_frac;

    /* scheduler */
    uint64_t next_line_cycle;     /* CPU cycle at which the next scanline starts */
    uint64_t frame_start_cycle;
    int cur_line;
    uint8_t irq;                  /* VDP /INT line */
    uint8_t recomp;               /* 1 = run recompiled code, 0 = interpreter only */
    uint8_t cheat_inf_health;     /* F1: player health ($C939) can't go down */
    uint16_t resume_pc;           /* PC of the next instruction while frame_callback runs (save states) */
    void (*frame_callback)(void); /* host: present, poll input, pace; called every frame */

    Z80 cpu;
    uint64_t frame;
} GG;

extern GG gg;
void gg_mapper_write(uint16_t addr, uint8_t v);

/* Memory map (Sega mapper).  Inline: every emulated load/store goes through these. */
static inline uint8_t gg_read8(uint16_t a)
{
    if (a < 0xC000) {
        unsigned bank = a < 0x0400 ? 0 : gg.slot[a >> 14];
        if (bank >= gg.rom_banks) bank %= gg.rom_banks;
        return gg.rom[bank * 0x4000u + (a & 0x3FFF)];
    }
    return gg.ram[a & 0x1FFF];
}
#define GG_PLAYER_HEALTH 0xC939      /* player 1 health, 240 = full ($C9B9 = opponent; $C824 is the HUD bar) */
static inline void gg_write8(uint16_t a, uint8_t v)
{
    if (a < 0xC000) return;          /* no cartridge RAM on this board */
    if (a == GG_PLAYER_HEALTH && gg.cheat_inf_health && v < gg.ram[a & 0x1FFF]) return;
    gg.ram[a & 0x1FFF] = v;
    if (a >= 0xFFFC) gg_mapper_write(a, v);
}

/* memory.c */
int  gg_load_rom(const char *path);
void gg_reset(void);
/* physical bank currently visible at a CPU address (for dispatch). */
unsigned gg_bank_at(uint16_t addr);
static inline uint16_t gg_read16(uint16_t a) { return (uint16_t)(gg_read8(a) | (gg_read8((uint16_t)(a + 1)) << 8)); }
static inline void gg_write16(uint16_t a, uint16_t v) { gg_write8(a, (uint8_t)v); gg_write8((uint16_t)(a + 1), (uint8_t)(v >> 8)); }
uint32_t gg_crc32(const uint8_t *p, size_t n);

/* io.c */
uint8_t gg_in(uint8_t port);
void gg_out(uint8_t port, uint8_t v);

/* vdp.c */
uint8_t vdp_read_data(void);
uint8_t vdp_read_status(void);
void vdp_write_data(uint8_t v);
void vdp_write_ctrl(uint8_t v);
uint8_t vdp_vcounter(void);
void vdp_render_line(int line);
void vdp_line_tick(int line);
void vdp_update_irq(void);

/* psg.c */
void psg_write(uint8_t v);
void psg_render(int16_t *out, int frames, int rate, uint64_t frame_start, uint64_t frame_cycles);

/* machine.c - scheduler.  The game owns the thread: gg_boot() never returns
 * and the host runs inside frame_callback once per frame. */
void gg_boot(void);
void gg_boot_resume(void);                   /* continue from a loaded save state */
extern void (*gg_step_hook)(Z80 *cpu);       /* debug: called before each interpreted insn */
void gg_lines(void);
void gg_check(Z80 *cpu, uint16_t next_pc);   /* scanline events + IRQs; between instructions */
void gg_take_irq(Z80 *cpu, uint16_t next_pc);
/* fast test used after every instruction (interpreter and generated code) */
#define GG_NEED_CHECK(z) ((z)->cycles >= gg.next_line_cycle || ((z)->iff1 && (gg.irq || (z)->ei_delay)))
#ifdef GG_TRACE
void gg_trace(Z80 *z, uint16_t next_pc);
#define GG_TRACE_HOOK(z, pc) gg_trace((z), (pc))
#else
#define GG_TRACE_HOOK(z, pc) ((void)0)
#endif

/* state.c - save states (taken inside frame_callback, i.e. between instructions) */
int gg_state_save(const char *path);
int gg_state_load(const char *path);         /* 0 = ok; machine replaced, call gg_boot_resume() */
int gg_state_read(const char *path);         /* validate + stage a state without touching the machine */
void gg_state_apply(void);                   /* replace the machine with the staged state */

/* dispatch.c — recompiled functions keyed by (bank, address) */
typedef void (*gg_fn)(Z80 *);
gg_fn gg_lookup(unsigned bank, uint16_t addr);
void  gg_call(Z80 *cpu, uint16_t addr);   /* run code at addr until it returns (return address already pushed) */
void  gg_jump(Z80 *cpu);
int   gg_resume(Z80 *cpu, uint16_t ret_pc, uint16_t sp_before);                  /* continue at cpu->pc (no push); returns when that code returns */
void  gg_log_miss(unsigned bank, uint16_t addr);

/* interp.c — fallback Z80 interpreter for code not yet recompiled */
void z80_init(void);
int  z80_step(Z80 *cpu);                  /* one instruction; returns T-states */
int  z80_irq(Z80 *cpu);                   /* maskable IRQ (IM 1); returns T-states taken */
void z80_run_until_return(Z80 *cpu, uint16_t sp_at_entry);

#endif
