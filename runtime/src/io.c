/* io.c - Game Gear I/O port decoding. */
#include "gg.h"

uint8_t gg_in(uint8_t port)
{
    if (port == 0x00) /* bit7 = START (active low), bit6 = region (1 = export) */
        return (uint8_t)((gg.start ? 0x00 : 0x80) | 0x40);
    if (port <= 0x06) return 0x00;           /* GG link/serial regs: unused here */
    if (port < 0x40) return 0xFF;
    if (port < 0x80) return (port & 1) ? 0x00 /* H counter */ : vdp_vcounter();
    if (port < 0xC0) return (port & 1) ? vdp_read_status() : vdp_read_data();
    if (port == 0xDC || port == 0xC0) return (uint8_t)(gg.pad | 0xC0);
    if (port == 0xDD || port == 0xC1) return 0xFF;
    return 0xFF;
}

void gg_out(uint8_t port, uint8_t v)
{
    if (port == 0x06) { gg.psg_stereo = v; return; }
    if (port <= 0x05) return;
    if (port < 0x40) return;                 /* memory/IO control: ignored */
    if (port < 0x80) { psg_write(v); return; }
    if (port < 0xC0) { if (port & 1) vdp_write_ctrl(v); else vdp_write_data(v); return; }
}
