/* state.c - save states.
 *
 * A state is the whole GG struct (CPU, RAM, VRAM, VDP, PSG, mapper, scheduler)
 * captured from inside the frame callback, where the machine sits between two
 * instructions.  Host pointers (ROM, callback) are kept from the running
 * session on load.  The header pins the ROM CRC and the struct layout so a
 * state from a different build/ROM is rejected instead of misloaded. */
#include "gg.h"
#include <stdio.h>
#include <string.h>

#define STATE_MAGIC   "MMPRGGST"
#define STATE_VERSION 1u

typedef struct {
    char magic[8];
    uint32_t version, rom_crc, struct_size, reserved;
} state_header;

int gg_state_save(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    state_header h;
    memcpy(h.magic, STATE_MAGIC, 8);
    h.version = STATE_VERSION;
    h.rom_crc = GG_ROM_CRC32;
    h.struct_size = (uint32_t)sizeof gg;
    h.reserved = 0;
    int ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(&gg, sizeof gg, 1, f) == 1;
    ok = (fclose(f) == 0) && ok;
    return ok ? 0 : -2;
}

static GG pending;           /* validated state waiting to be applied */

int gg_state_read(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    state_header h;
    int ok = fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, STATE_MAGIC, 8) &&
             h.version == STATE_VERSION && h.rom_crc == GG_ROM_CRC32 &&
             h.struct_size == sizeof gg && fread(&pending, sizeof pending, 1, f) == 1;
    fclose(f);
    return ok ? 0 : -2;
}

void gg_state_apply(void)
{
    GG *t = &pending;
    /* keep this session's host-side fields */
    t->rom = gg.rom; t->rom_size = gg.rom_size; t->rom_banks = gg.rom_banks;
    t->frame_callback = gg.frame_callback;
    t->recomp = gg.recomp;
    t->cheat_inf_health = gg.cheat_inf_health;
    gg = *t;
}

int gg_state_load(const char *path)
{
    int rc = gg_state_read(path);
    if (rc == 0) gg_state_apply();
    return rc;
}
