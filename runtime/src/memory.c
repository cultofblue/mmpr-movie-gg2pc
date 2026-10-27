/* memory.c - ROM loading and the Sega mapper memory map. */
#include "gg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

GG gg;

uint32_t gg_crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

int gg_load_rom(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n % 0x4000) { fclose(f); return -2; }
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return -3; }
    fclose(f);
    gg.rom = buf;
    gg.rom_size = (size_t)n;
    gg.rom_banks = (unsigned)(n / 0x4000);
    if (gg_crc32(buf, (size_t)n) != GG_ROM_CRC32) return 1; /* loaded, but not the expected dump */
    return 0;
}

void gg_reset(void)
{
    /* keep the cartridge and host hooks, clear everything else */
    const uint8_t *rom = gg.rom;
    size_t rom_size = gg.rom_size;
    unsigned banks = gg.rom_banks;
    void (*cb)(void) = gg.frame_callback;
    uint8_t recomp = gg.recomp;
    uint8_t cheat = gg.cheat_inf_health;
    memset(&gg, 0, sizeof gg);
    gg.rom = rom; gg.rom_size = rom_size; gg.rom_banks = banks;
    gg.frame_callback = cb; gg.recomp = recomp; gg.cheat_inf_health = cheat;

    gg.slot[0] = 0; gg.slot[1] = 1; gg.slot[2] = 2;
    gg.cpu.sp = 0xDFF0;
    gg.cpu.af = 0xFFFF;
    gg.cpu.pc = 0x0000;
    gg.pad = 0xFF;
    gg.psg_stereo = 0xFF;
    gg.psg_lfsr = 0x8000;
    memset(gg.psg_vol, 0x0F, sizeof gg.psg_vol);
}

unsigned gg_bank_at(uint16_t addr)
{
    if (addr < 0x0400) return 0;
    if (addr < 0xC000) return gg.slot[addr >> 14] % gg.rom_banks;
    return 0xFFFF; /* RAM */
}

void gg_mapper_write(uint16_t a, uint8_t v)
{
    switch (a) {
    case 0xFFFC: gg.mapper_ctrl = v; break;
    case 0xFFFD: gg.slot[0] = v; break;
    case 0xFFFE: gg.slot[1] = v; break;
    case 0xFFFF: gg.slot[2] = v; break;
    }
}
