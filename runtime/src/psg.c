/* psg.c - SN76489 (Sega variant) with Game Gear stereo.
 * Register writes are time-stamped in CPU cycles and replayed while the frame's
 * audio is rendered, so timing inside a frame is preserved. */
#include "gg.h"
#include <math.h>
#include <string.h>

#define PSG_CLOCK 3579545.0
#define MAX_EVENTS 4096

typedef struct { uint64_t cycle; uint8_t v; } psg_event;
static psg_event events[MAX_EVENTS];
static int n_events;

#define counter  gg.psg_counter
#define output   gg.psg_output
#define lfsr     gg.psg_lfsr
#define tick_acc gg.psg_tick_acc
static int16_t vol_table[16];
static int vol_ready;

static void reg_write(uint8_t v)
{
    if (v & 0x80) {
        gg.psg_latch = (v >> 4) & 7;
        int ch = gg.psg_latch >> 1;
        if (gg.psg_latch & 1) gg.psg_vol[ch] = v & 0x0F;
        else if (ch < 3) gg.psg_tone[ch] = (uint16_t)((gg.psg_tone[ch] & 0x3F0) | (v & 0x0F));
        else { gg.psg_noise = v & 0x07; lfsr = 0x8000; }
    } else {
        int ch = gg.psg_latch >> 1;
        if (gg.psg_latch & 1) gg.psg_vol[ch] = v & 0x0F;
        else if (ch < 3) gg.psg_tone[ch] = (uint16_t)((gg.psg_tone[ch] & 0x00F) | ((v & 0x3F) << 4));
        else { gg.psg_noise = v & 0x07; lfsr = 0x8000; }
    }
}

void psg_write(uint8_t v)
{
    if (n_events < MAX_EVENTS) {
        events[n_events].cycle = gg.cpu.cycles;
        events[n_events].v = v;
        n_events++;
    } else {
        reg_write(v);
    }
}

static void tick(void)
{
    for (int ch = 0; ch < 3; ch++) {
        if (--counter[ch] <= 0) {
            counter[ch] = gg.psg_tone[ch] ? gg.psg_tone[ch] : 1;
            output[ch] = gg.psg_tone[ch] <= 1 ? 1 : !output[ch];
        }
    }
    if (--counter[3] <= 0) {
        int r = gg.psg_noise & 3;
        counter[3] = r == 3 ? (gg.psg_tone[2] ? gg.psg_tone[2] : 1) : (0x10 << r);
        gg.psg_nflip = !gg.psg_nflip;
        if (gg.psg_nflip) {
            int fb = (gg.psg_noise & 4) ? (((lfsr & 1) ^ ((lfsr >> 3) & 1))) : (lfsr & 1);
            lfsr = (uint16_t)((lfsr >> 1) | (fb << 15));
            output[3] = lfsr & 1;
        }
    }
}

void psg_render(int16_t *out, int frames, int rate, uint64_t frame_start, uint64_t frame_cycles)
{
    if (!vol_ready) {
        for (int i = 0; i < 15; i++) vol_table[i] = (int16_t)(8000.0 * pow(10.0, -0.1 * i));
        vol_table[15] = 0;
        vol_ready = 1;
    }
    double ticks_per_sample = PSG_CLOCK / 16.0 / rate;
    int ev = 0;
    for (int s = 0; s < frames; s++) {
        uint64_t at = frame_start + (uint64_t)((double)frame_cycles * s / frames);
        while (ev < n_events && events[ev].cycle <= at) reg_write(events[ev++].v);
        /* average the generator over the sample period to reduce aliasing */
        tick_acc += ticks_per_sample;
        int nt = (int)tick_acc;
        tick_acc -= nt;
        int accL = 0, accR = 0;
        for (int t = 0; t < nt; t++) {
            tick();
            for (int ch = 0; ch < 4; ch++) {
                int a = output[ch] ? vol_table[gg.psg_vol[ch] & 15] : -vol_table[gg.psg_vol[ch] & 15];
                if (gg.psg_stereo & (0x10 << ch)) accL += a;
                if (gg.psg_stereo & (0x01 << ch)) accR += a;
            }
        }
        if (nt) { accL /= nt; accR /= nt; }
        out[s * 2] = (int16_t)accL;
        out[s * 2 + 1] = (int16_t)accR;
    }
    while (ev < n_events) reg_write(events[ev++].v);
    n_events = 0;
}
