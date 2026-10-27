/* headless.c - run the game without a window: scripted input, screenshots,
 * WAV capture and per-frame state hashes (for the recomp-vs-interp diff test).
 *
 * headless ROM FRAMES [options]
 *   -i            interpreter only (default: recompiled code)
 *   -s LIST       screenshot frames, comma separated -> OUT/fNNNNN.ppm
 *   -k SCRIPT     input script "frame:keys;frame:keys" keys = U D L R 1 2 S, '-' = none
 *   -o DIR        output directory (default .)
 *   -w FILE       write all audio to a WAV file
 *   -h FILE       write one state hash per frame to FILE
 */
#include "gg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int frames, frame_no;
static const char *shots = "", *script = "", *outdir = ".";
static FILE *wav, *hashes, *ramdump;
static int save_at = -1;
static const char *save_path;
static uint32_t wav_samples;
extern uint64_t gg_interp_steps;

/* ---- dynamic coverage (-c): executed code + dynamic entry points ---- */
static FILE *covf;
static uint8_t cov_exec[32][0x4000];     /* per ROM bank: 1 = insn start executed */
static uint8_t cov_entry[32][0x4000];    /* 1 = call/rst/jp(rr) target, 2 = odd ret target */
static uint8_t ram_exec[0x2000];
static int pend_kind;                    /* what the previous instruction was */
static uint16_t pend_next;               /* fall-through address of previous insn */
/* shadow return stack: where each return address lives on the Z80 stack */
static struct { uint16_t loc, addr; } rs[1024];
static int rs_n;

static void mark(uint16_t pc, int entry)
{
    if (pc >= 0xC000) { ram_exec[pc & 0x1FFF] = 1; return; }
    unsigned b = gg_bank_at(pc) & 31;
    cov_exec[b][pc & 0x3FFF] = 1;
    if (entry == 1 || (entry == 2 && !cov_entry[b][pc & 0x3FFF])) cov_entry[b][pc & 0x3FFF] = (uint8_t)entry;
}

static void rs_push(uint16_t loc, uint16_t addr)
{
    while (rs_n > 0 && (int16_t)(rs[rs_n - 1].loc - loc) <= 0) rs_n--;   /* stale/overwritten */
    if (rs_n < 1024) { rs[rs_n].loc = loc; rs[rs_n].addr = addr; rs_n++; }
}

static void cov_hook(Z80 *z)
{
    uint16_t pc = z->pc;
    int entry = 0;
    if (pend_kind && pc == pend_next && pend_kind != 2) pend_kind = 0;   /* conditional not taken */
    if (pc == 0x0038 && pend_kind != 1) {          /* interrupt */
        rs_push(z->sp, gg_read16(z->sp));
        pend_kind = 0;
        entry = 1;
    }
    if (pend_kind == 1) {                          /* call / rst */
        entry = 1;
        rs_push(z->sp, gg_read16(z->sp));
    } else if (pend_kind == 2) {                   /* jp (hl/ix/iy) */
        entry = 1;
    } else if (pend_kind == 3) {                   /* ret: was it to the expected caller? */
        uint16_t loc = (uint16_t)(z->sp - 2);
        while (rs_n > 0 && (int16_t)(rs[rs_n - 1].loc - loc) < 0) rs_n--;   /* unwound frames */
        if (rs_n > 0 && rs[rs_n - 1].loc == loc && rs[rs_n - 1].addr == pc) rs_n--;
        else entry = 2;
    }
    mark(pc, entry);
    uint8_t op = gg_read8(pc), op2 = gg_read8((uint16_t)(pc + 1));
    pend_kind = 0;
    pend_next = 0xFFFF;
    if (op == 0xCD || (op & 0xC7) == 0xC4) { pend_kind = 1; if (op != 0xCD) pend_next = (uint16_t)(pc + 3); }
    else if ((op & 0xC7) == 0xC7) pend_kind = 1;
    else if (op == 0xE9 || ((op == 0xDD || op == 0xFD) && op2 == 0xE9)) pend_kind = 2;
    else if (op == 0xC9 || (op & 0xC7) == 0xC0 || (op == 0xED && (op2 & 0xC7) == 0x45)) {
        pend_kind = 3;
        if ((op & 0xC7) == 0xC0) pend_next = (uint16_t)(pc + 1);
    }
}

static void cov_write(void)
{
    if (!covf) return;
    for (int b = 0; b < 32; b++)
        for (int a = 0; a < 0x4000; a++) {
            if (!cov_exec[b][a]) continue;
            unsigned cpu = b == 0 ? a : b == 1 ? 0x4000 + a : 0x8000 + a;
            fprintf(covf, "%c %02X %04X\n", cov_entry[b][a] == 1 ? 'E' : cov_entry[b][a] == 2 ? 'W' : 'X', b, cpu);
        }
    for (int a = 0; a < 0x2000; a++) if (ram_exec[a]) fprintf(covf, "R -- %04X\n", 0xC000 + a);
    fclose(covf);
}

static void save_ppm(int frame)
{
    char path[512];
    snprintf(path, sizeof path, "%s/f%05d.ppm", outdir, frame);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", GG_SCREEN_W, GG_SCREEN_H);
    for (int i = 0; i < GG_SCREEN_W * GG_SCREEN_H; i++) {
        uint32_t p = gg.framebuffer[i];
        uint8_t rgb[3] = { (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void apply_keys(const char *k)
{
    uint8_t p = 0xFF; int st = 0;
    for (; *k && *k != ';'; k++) switch (*k) {
        case 'U': p &= ~0x01; break; case 'D': p &= ~0x02; break;
        case 'L': p &= ~0x04; break; case 'R': p &= ~0x08; break;
        case '1': p &= ~0x10; break; case '2': p &= ~0x20; break;
        case 'S': st = 1; break;
    }
    gg.pad = p; gg.start = (uint8_t)st;
}

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}

static void finish(void)
{
    if (wav) {
        uint32_t data = wav_samples * 4, riff = 36 + data, rate = GG_AUDIO_RATE, br = GG_AUDIO_RATE * 4, sz = 16;
        uint16_t pcm = 1, ch = 2, ba = 4, bits = 16;
        fseek(wav, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, wav); fwrite(&riff, 4, 1, wav); fwrite("WAVEfmt ", 1, 8, wav);
        fwrite(&sz, 4, 1, wav); fwrite(&pcm, 2, 1, wav); fwrite(&ch, 2, 1, wav); fwrite(&rate, 4, 1, wav);
        fwrite(&br, 4, 1, wav); fwrite(&ba, 2, 1, wav); fwrite(&bits, 2, 1, wav);
        fwrite("data", 1, 4, wav); fwrite(&data, 4, 1, wav); fclose(wav);
    }
    if (hashes) fclose(hashes);
    cov_write();
    fprintf(stderr, "%s: %d frames, %llu cycles, pc=%04X slot2=%02X state=%02X, fallback insns=%llu\n",
            gg.recomp ? "recomp" : "interp", frame_no, (unsigned long long)gg.cpu.cycles,
            gg.cpu.pc, gg.slot[2], gg.ram[0x410], (unsigned long long)gg_interp_steps);
    exit(0);
}

static void on_frame(void)
{
    int f = frame_no;   /* frame that just finished */
    if (wav) { fwrite(gg.audio, 4, (size_t)gg.audio_samples, wav); wav_samples += (uint32_t)gg.audio_samples; }
    if (ramdump && f >= (getenv("RAM_FROM") ? atoi(getenv("RAM_FROM")) : 0)) fwrite(gg.ram, 1, sizeof gg.ram, ramdump);
    if (hashes) {
        uint64_t h = 0xcbf29ce484222325ull;
        h = fnv(h, gg.ram, sizeof gg.ram);
        h = fnv(h, gg.vram, sizeof gg.vram);
        h = fnv(h, gg.cram, sizeof gg.cram);
        h = fnv(h, gg.framebuffer, sizeof gg.framebuffer);
        fprintf(hashes, "%d %016llx %llu\n", f, (unsigned long long)h, (unsigned long long)gg.cpu.cycles);
    }
    for (const char *p = shots; *p; ) {
        if (atoi(p) == f) save_ppm(f);
        p = strchr(p, ',');
        if (!p) break;
        p++;
    }
    if (f == save_at && save_path) {
        if (gg_state_save(save_path) == 0) fprintf(stderr, "saved state after frame %d (pc=%04X)\n", f, gg.resume_pc);
    }
    frame_no++;
    if (frame_no >= frames) finish();
    /* input for the next frame */
    for (const char *s = script; s && *s; ) {
        int ef = atoi(s);
        const char *c = strchr(s, ':');
        if (!c) break;
        if (ef == frame_no) apply_keys(c + 1);
        s = strchr(c, ';');
        if (s) s++;
    }
}

int main(int argc, char **argv)
{
    const char *load_path = NULL;
    if (argc < 3) { fprintf(stderr, "usage: headless ROM FRAMES [-i] [-s list] [-k script] [-o dir] [-w wav] [-h hashes]\n"); return 2; }
    if (gg_load_rom(argv[1]) < 0) { fprintf(stderr, "rom load failed\n"); return 1; }
    frames = atoi(argv[2]);
    gg.recomp = 1;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-i")) gg.recomp = 0;
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) shots = argv[++i];
        else if (!strcmp(argv[i], "-k") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) { wav = fopen(argv[++i], "wb"); if (wav) fseek(wav, 44, SEEK_SET); }
        else if (!strcmp(argv[i], "-h") && i + 1 < argc) hashes = fopen(argv[++i], "w");
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) ramdump = fopen(argv[++i], "wb");
        else if (!strcmp(argv[i], "-g")) gg.cheat_inf_health = 1;
        else if (!strcmp(argv[i], "-S") && i + 2 < argc) { save_at = atoi(argv[++i]); save_path = argv[++i]; }
        else if (!strcmp(argv[i], "-L") && i + 1 < argc) load_path = argv[++i];
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) { covf = fopen(argv[++i], "w"); gg_step_hook = cov_hook; }
    }
    z80_init();
    gg_reset();
    gg.frame_callback = on_frame;
    if (load_path) {
        if (gg_state_load(load_path) != 0) { fprintf(stderr, "state load failed\n"); return 1; }
        frame_no = (int)gg.frame;
        fprintf(stderr, "loaded state at frame %d (pc=%04X)\n", frame_no, gg.resume_pc);
        gg_boot_resume();
    }
    gg_boot();
    return 0;
}
