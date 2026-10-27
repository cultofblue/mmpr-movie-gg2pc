/* vdp.c - Game Gear VDP (SMS Mode 4).  Port protocol is complete; the
 * scanline renderer is the next milestone (see docs/ROADMAP.md). */
#include "gg.h"

static uint32_t cram_rgb(int idx)
{
    uint16_t c = (uint16_t)(gg.cram[idx * 2] | (gg.cram[idx * 2 + 1] << 8));
    uint32_t r = (c & 0xF) * 17, g = ((c >> 4) & 0xF) * 17, b = ((c >> 8) & 0xF) * 17;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

uint8_t vdp_read_status(void)
{
    uint8_t s = gg.vdp_status;
    gg.vdp_status &= 0x1F;
    gg.vdp_irq_line_pending = 0;
    gg.vdp_latch_pending = 0;
    vdp_update_irq();
    return s;
}

uint8_t vdp_read_data(void)
{
    uint8_t v = gg.vdp_buffer;
    gg.vdp_buffer = gg.vram[gg.vdp_addr & 0x3FFF];
    gg.vdp_addr = (gg.vdp_addr + 1) & 0x3FFF;
    gg.vdp_latch_pending = 0;
    return v;
}

void vdp_write_data(uint8_t v)
{
    gg.vdp_latch_pending = 0;
    if (gg.vdp_code == 3) {
        /* GG CRAM: even writes latch, odd writes commit the 12-bit colour */
        if (gg.vdp_addr & 1) {
            gg.cram[(gg.vdp_addr - 1) & 0x3F] = gg.vdp_latch;
            gg.cram[gg.vdp_addr & 0x3F] = v & 0x0F;
        } else {
            gg.vdp_latch = v;
        }
    } else {
        gg.vram[gg.vdp_addr & 0x3FFF] = v;
    }
    gg.vdp_buffer = v;
    gg.vdp_addr = (gg.vdp_addr + 1) & 0x3FFF;
}

void vdp_write_ctrl(uint8_t v)
{
    static uint8_t first;
    if (!gg.vdp_latch_pending) {
        first = v;
        gg.vdp_addr = (gg.vdp_addr & 0x3F00) | v;
        gg.vdp_latch_pending = 1;
        return;
    }
    gg.vdp_latch_pending = 0;
    gg.vdp_code = v >> 6;
    gg.vdp_addr = (uint16_t)(((v & 0x3F) << 8) | first);
    if (gg.vdp_code == 0) {
        gg.vdp_buffer = gg.vram[gg.vdp_addr];
        gg.vdp_addr = (gg.vdp_addr + 1) & 0x3FFF;
    } else if (gg.vdp_code == 2) {
        gg.vdp_reg[v & 0x0F] = first;
        vdp_update_irq();
    }
}

uint8_t vdp_vcounter(void)
{
    int l = gg.vdp_line;
    return (uint8_t)(l <= 0xDA ? l : l - 6);
}

void vdp_update_irq(void)
{
    int frame = (gg.vdp_status & 0x80) && (gg.vdp_reg[1] & 0x20);
    int line = gg.vdp_irq_line_pending && (gg.vdp_reg[0] & 0x10);
    gg.irq = (uint8_t)(frame || line);
}

/* Line counter: runs on lines 0..192, reloaded from R10 elsewhere. */
void vdp_line_tick(int line)
{
    if (line <= 192) {
        if (gg.vdp_line_counter == 0) {
            gg.vdp_line_counter = gg.vdp_reg[10];
            gg.vdp_irq_line_pending = 1;
        } else {
            gg.vdp_line_counter--;
        }
    } else {
        gg.vdp_line_counter = gg.vdp_reg[10];
    }
    if (line == 192) gg.vdp_status |= 0x80;
    if (line == 0) gg.vdp_vscroll_latch = gg.vdp_reg[9];
    vdp_update_irq();
}

static inline int tile_pixel(uint16_t tile_addr, int row, int col)
{
    const uint8_t *p = &gg.vram[(tile_addr + row * 4) & 0x3FFF];
    int sh = 7 - col;
    return ((p[0] >> sh) & 1) | (((p[1] >> sh) & 1) << 1) | (((p[2] >> sh) & 1) << 2) | (((p[3] >> sh) & 1) << 3);
}

void vdp_render_line(int line)
{
    /* GG shows lines 24..167 of the 192-line SMS frame, columns 48..207. */
    int y = line - 24;
    if (y < 0 || y >= GG_SCREEN_H) return;
    uint32_t *row = &gg.framebuffer[y * GG_SCREEN_W];
    uint32_t pal[32];
    for (int i = 0; i < 32; i++) pal[i] = cram_rgb(i);
    uint32_t backdrop = pal[16 + (gg.vdp_reg[7] & 0x0F)];

    if (!(gg.vdp_reg[1] & 0x40)) {   /* display blanked */
        for (int x = 0; x < GG_SCREEN_W; x++) row[x] = backdrop;
        return;
    }

    uint8_t bgcol[256];   /* palette index 0..31, bit7 = priority + opaque */
    uint16_t nt = (uint16_t)((gg.vdp_reg[2] & 0x0E) << 10);
    int hscroll = (gg.vdp_reg[0] & 0x40) && line < 16 ? 0 : gg.vdp_reg[8];
    for (int x = 0; x < 256; x++) {
        int vscroll = (gg.vdp_reg[0] & 0x80) && x >= 192 ? 0 : gg.vdp_vscroll_latch;
        int sy = (line + vscroll) % 224;
        int sx = (x - hscroll) & 0xFF;
        uint16_t e = (uint16_t)(gg.vram[(nt + ((sy >> 3) * 32 + (sx >> 3)) * 2) & 0x3FFF] |
                                (gg.vram[(nt + ((sy >> 3) * 32 + (sx >> 3)) * 2 + 1) & 0x3FFF] << 8));
        int tr = sy & 7, tc = sx & 7;
        if (e & 0x0400) tr = 7 - tr;
        if (e & 0x0200) tc = 7 - tc;
        int c = tile_pixel((uint16_t)((e & 0x1FF) * 32), tr, tc);
        uint8_t v = (uint8_t)(c | ((e & 0x0800) ? 16 : 0));
        if ((e & 0x1000) && c) v |= 0x80;
        bgcol[x] = v;
    }

    /* sprites */
    uint8_t spcol[256] = {0};
    uint8_t spset[256] = {0};
    uint16_t sat = (uint16_t)((gg.vdp_reg[5] & 0x7E) << 7);
    uint16_t sbase = (gg.vdp_reg[6] & 0x04) ? 0x2000 : 0;
    int h = (gg.vdp_reg[1] & 0x02) ? 16 : 8;
    int zoom = (gg.vdp_reg[1] & 0x01) ? 2 : 1;
    int count = 0;
    for (int i = 0; i < 64; i++) {
        int sy = gg.vram[sat + i];
        if (sy == 0xD0) break;
        int top = sy + 1;
        if (top > 240) top -= 256;
        if (line < top || line >= top + h * zoom) continue;
        if (++count > 8) { gg.vdp_status |= 0x40; break; }
        int sx = gg.vram[(sat + 0x80 + i * 2) & 0x3FFF];
        if (gg.vdp_reg[0] & 0x08) sx -= 8;
        int n = gg.vram[(sat + 0x81 + i * 2) & 0x3FFF];
        if (h == 16) n &= 0xFE;
        int r = (line - top) / zoom;
        uint16_t ta = (uint16_t)(sbase + n * 32 + (r >= 8 ? 32 : 0));
        for (int px = 0; px < 8 * zoom; px++) {
            int x = sx + px;
            if (x < 0 || x > 255) continue;
            int c = tile_pixel(ta, r & 7, px / zoom);
            if (!c) continue;
            if (spset[x]) { gg.vdp_status |= 0x20; continue; }
            spset[x] = 1;
            spcol[x] = (uint8_t)(16 + c);
        }
    }

    for (int gx = 0; gx < GG_SCREEN_W; gx++) {
        int x = gx + 48;
        uint8_t b = bgcol[x];
        int idx;
        if (spset[x] && !(b & 0x80)) idx = spcol[x];
        else idx = b & 0x1F;
        row[gx] = pal[idx];
    }
}
