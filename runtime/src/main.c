/* main.c - Windows/SDL2 host for MMPR: The Movie (Game Gear).
 *
 * The recompiled game owns the thread (gg_boot never returns); everything the
 * host does - present, input, audio, pacing, save states, filters - happens in
 * host_frame(), which the scheduler calls once per emulated frame. */
#include "gg.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <setjmp.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir((p), 0755)
#endif
#include "font3x5.h"
#include "../../res/icon_rgba.h"

#define SCALE 4
#define ROM_NAME "Mighty Morphin Power Rangers - The Movie (USA, Europe, Brazil) (En).gg"
#define TITLE "Mighty Morphin Power Rangers: The Movie (Game Gear)"
#define FRAME_HZ (3579545.0 / (228.0 * 262.0))   /* 59.92 */
#define FRAME_BYTES (736 * 4)
#define FW (GG_SCREEN_W * 4)                      /* filtered output size */
#define FH (GG_SCREEN_H * 4)

enum { FILTER_NONE, FILTER_LCD, FILTER_CRT, FILTER_COUNT };
static const char *filter_names[] = { "SCREEN FILTER: NONE", "SCREEN FILTER: LCD", "SCREEN FILTER: CRT" };

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex, *tex4;
static SDL_AudioDeviceID audio;
static SDL_GameController *pad;
static Uint64 perf_freq;
static double next_tick, frame_ticks;
static int paused, filter, slot, fullscreen;
static jmp_buf reset_jmp;
static char base_dir[1024];
static uint32_t filtered[FW * FH];
static uint32_t lcd_prev[GG_SCREEN_W * GG_SCREEN_H];

/* ------------------------------------------------------------ files */
static void path_in_base(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s%s", base_dir, name);
}

static void state_path(char *out, size_t n, int s)
{
    char name[64];
    snprintf(name, sizeof name, "saves/slot%d.state", s);
    path_in_base(out, n, name);
}

static void load_settings(void)
{
    char p[1100];
    path_in_base(p, sizeof p, "settings.ini");
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        int v;
        if (sscanf(line, "filter=%d", &v) == 1 && v >= 0 && v < FILTER_COUNT) filter = v;
        else if (sscanf(line, "slot=%d", &v) == 1 && v >= 0 && v <= 9) slot = v;
        else if (sscanf(line, "fullscreen=%d", &v) == 1) fullscreen = v != 0;
    }
    fclose(f);
}

static void save_settings(void)
{
    char p[1100];
    path_in_base(p, sizeof p, "settings.ini");
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "filter=%d\nslot=%d\nfullscreen=%d\n", filter, slot, fullscreen);
    fclose(f);
}

static const char *find_rom(const char *arg)
{
    static char path[1100];
    if (arg) return arg;
    const char *names[] = { ROM_NAME, "mmpr_movie.gg", "rom/mmpr_movie.gg", "../" ROM_NAME };
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) {
        path_in_base(path, sizeof path, names[i]);
        FILE *f = fopen(path, "rb");
        if (f) { fclose(f); return path; }
    }
    /* any .gg file next to the exe would need a directory scan; keep it simple */
    return ROM_NAME;
}

/* ------------------------------------------------------------ input */
static void read_input(void)
{
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    int up = k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W];
    int dn = k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_S];
    int lf = k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A];
    int rt = k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D];
    int b1 = k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_J];
    int b2 = k[SDL_SCANCODE_X] || k[SDL_SCANCODE_K];
    int st = k[SDL_SCANCODE_RETURN];
    if (pad) {
        up |= SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP);
        dn |= SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        lf |= SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        rt |= SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        int ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
        int ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
        lf |= ax < -16000; rt |= ax > 16000; up |= ay < -16000; dn |= ay > 16000;
        b1 |= SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A) |
              SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X);
        b2 |= SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B) |
              SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y);
        st |= SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START);
    }
    uint8_t p = 0xFF;
    if (up) p &= ~0x01;
    if (dn) p &= ~0x02;
    if (lf) p &= ~0x04;
    if (rt) p &= ~0x08;
    if (b1) p &= ~0x10;
    if (b2) p &= ~0x20;
    gg.pad = p;
    gg.start = (uint8_t)(st ? 1 : 0);
}

/* ------------------------------------------------------------ on-screen messages */
static char toast_text[48];
static int toast_frames;

static void toast(const char *msg)
{
    snprintf(toast_text, sizeof toast_text, "%s", msg);
    toast_frames = 120;
}

static void draw_toast(void)
{
    if (!toast_frames) return;
    toast_frames--;
    int len = (int)strlen(toast_text), w = len * 4 + 3, x0 = (GG_SCREEN_W - w) / 2, y0 = GG_SCREEN_H - 30;
    SDL_Rect bg = { x0, y0, w, 9 };
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderFillRect(ren, &bg);
    SDL_SetRenderDrawColor(ren, 255, 230, 40, 255);
    for (int i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)toast_text[i];
        if (ch >= 'a' && ch <= 'z') ch = (unsigned char)(ch - 32);
        unsigned short g = (ch >= 32 && ch < 96) ? font3x5[ch - 32] : 0;
        for (int r = 0; r < 5; r++)
            for (int c = 0; c < 3; c++)
                if (g & (1 << (14 - (r * 3 + c))))
                    SDL_RenderDrawPoint(ren, x0 + 2 + i * 4 + c, y0 + 2 + r);
    }
}

/* ------------------------------------------------------------ screen filters */
static inline uint32_t rgb(int r, int g, int b)
{
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Game Gear LCD: slow pixel response (ghosting) and a visible pixel grid. */
static void filter_lcd(void)
{
    for (int y = 0; y < GG_SCREEN_H; y++)
        for (int x = 0; x < GG_SCREEN_W; x++) {
            uint32_t c = gg.framebuffer[y * GG_SCREEN_W + x], p = lcd_prev[y * GG_SCREEN_W + x];
            int r = (int)(((c >> 16) & 255) * 6 + ((p >> 16) & 255) * 4) / 10;
            int g = (int)(((c >> 8) & 255) * 6 + ((p >> 8) & 255) * 4) / 10;
            int b = (int)((c & 255) * 6 + (p & 255) * 4) / 10;
            lcd_prev[y * GG_SCREEN_W + x] = rgb(r, g, b);
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    int k = (sx == 3 || sy == 3) ? 70 : 100;
                    filtered[(y * 4 + sy) * FW + x * 4 + sx] = rgb(r * k / 100, g * k / 100, b * k / 100);
                }
        }
}

/* CRT: scanlines, slight horizontal bleed and an RGB aperture-grille mask. */
static void filter_crt(void)
{
    static const int scan[4] = { 88, 104, 100, 46 };
    static const int mask[3][3] = { { 110, 84, 84 }, { 84, 110, 84 }, { 84, 84, 110 } };
    for (int y = 0; y < GG_SCREEN_H; y++) {
        const uint32_t *row = &gg.framebuffer[y * GG_SCREEN_W];
        for (int x = 0; x < GG_SCREEN_W; x++) {
            uint32_t c = row[x], l = row[x > 0 ? x - 1 : x], rr = row[x < GG_SCREEN_W - 1 ? x + 1 : x];
            for (int sx = 0; sx < 4; sx++) {
                /* blend with the neighbour on the near side at the pixel edges */
                uint32_t n = sx == 0 ? l : sx == 3 ? rr : c;
                int wc = (sx == 0 || sx == 3) ? 3 : 4, wn = 4 - wc;
                int r = (int)(((c >> 16) & 255) * wc + ((n >> 16) & 255) * wn) / 4;
                int g = (int)(((c >> 8) & 255) * wc + ((n >> 8) & 255) * wn) / 4;
                int b = (int)((c & 255) * wc + (n & 255) * wn) / 4;
                const int *m = mask[(x * 4 + sx) % 3];
                for (int sy = 0; sy < 4; sy++) {
                    int k = scan[sy];
                    filtered[(y * 4 + sy) * FW + x * 4 + sx] =
                        rgb(r * k * m[0] / 9000, g * k * m[1] / 9000, b * k * m[2] / 9000);
                }
            }
        }
    }
}

static void present(void)
{
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    if (filter == FILTER_NONE) {
        SDL_UpdateTexture(tex, NULL, gg.framebuffer, GG_SCREEN_W * 4);
        SDL_RenderCopy(ren, tex, NULL, NULL);
    } else {
        if (filter == FILTER_LCD) filter_lcd(); else filter_crt();
        SDL_UpdateTexture(tex4, NULL, filtered, FW * 4);
        SDL_RenderCopy(ren, tex4, NULL, NULL);
    }
    draw_toast();
    SDL_RenderPresent(ren);
}

/* ------------------------------------------------------------ events */
static void quit_now(void)
{
    save_settings();
    if (pad) SDL_GameControllerClose(pad);
    SDL_Quit();
    exit(0);
}

static void update_title(void)
{
    char t[160];
    snprintf(t, sizeof t, "%s  [slot %d]%s", TITLE, slot, gg.cheat_inf_health ? "  [Infinite Health ON]" : "");
    SDL_SetWindowTitle(win, t);
}

static void do_save(void)
{
    char p[1100], msg[48];
    path_in_base(p, sizeof p, "saves");
    make_dir(p);
    state_path(p, sizeof p, slot);
    if (gg_state_save(p) == 0) snprintf(msg, sizeof msg, "STATE %d SAVED", slot);
    else snprintf(msg, sizeof msg, "SAVE FAILED");
    toast(msg);
}

static void do_load(void)
{
    char p[1100], msg[48];
    state_path(p, sizeof p, slot);
    int rc = gg_state_read(p);
    if (rc != 0) {
        snprintf(msg, sizeof msg, rc == -1 ? "SLOT %d IS EMPTY" : "SLOT %d: INCOMPATIBLE STATE", slot);
        toast(msg);
        return;
    }
    snprintf(msg, sizeof msg, "STATE %d LOADED", slot);
    toast(msg);
    paused = 0;
    longjmp(reset_jmp, 2);          /* leave the running game's call stack */
}

static void poll_events(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) quit_now();
        else if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(e.cdevice.which);
        else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            SDL_Keycode k = e.key.keysym.sym;
            char msg[48];
            if (k == SDLK_ESCAPE) quit_now();
            else if (k == SDLK_F11 || (k == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
                fullscreen = !fullscreen;
                SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                save_settings();
            } else if (k == SDLK_p) {
                paused = !paused;
                toast(paused ? "PAUSED" : "");
                if (!paused) toast_frames = 0;
            } else if (k == SDLK_F1) {
                gg.cheat_inf_health = !gg.cheat_inf_health;
                update_title();
                toast(gg.cheat_inf_health ? "INFINITE HEALTH ON" : "INFINITE HEALTH OFF");
            } else if (k == SDLK_F2) {
                toast("RESET");
                longjmp(reset_jmp, 1);
            } else if (k == SDLK_F3) {
                filter = (filter + 1) % FILTER_COUNT;
                memcpy(lcd_prev, gg.framebuffer, sizeof lcd_prev);
                toast(filter_names[filter]);
                save_settings();
            } else if (k == SDLK_F5) {
                do_save();
            } else if (k == SDLK_F6) {
                slot = (slot + 1) % 10;
                snprintf(msg, sizeof msg, "SAVE SLOT %d", slot);
                toast(msg);
                update_title();
                save_settings();
            } else if (k == SDLK_F7) {
                do_load();
            }
        }
    }
}

static void pace(int fast)
{
    if (fast) { next_tick = (double)SDL_GetPerformanceCounter(); return; }
    double adj = 0;
    if (audio) {
        Uint32 q = SDL_GetQueuedAudioSize(audio);
        if (q < FRAME_BYTES * 2) adj = -0.02 * frame_ticks;
        else if (q > FRAME_BYTES * 5) adj = 0.02 * frame_ticks;
    }
    next_tick += frame_ticks + adj;
    double now = (double)SDL_GetPerformanceCounter();
    if (next_tick > now) {
        double ms = (next_tick - now) * 1000.0 / (double)perf_freq;
        if (ms > 2) SDL_Delay((Uint32)(ms - 1.5));
        while ((double)SDL_GetPerformanceCounter() < next_tick) { }
    } else if (now - next_tick > frame_ticks * 4) {
        next_tick = now;
    }
}

/* Called by the scheduler at the end of every emulated frame. */
static void host_frame(void)
{
    int fast = SDL_GetKeyboardState(NULL)[SDL_SCANCODE_TAB];
    if (audio && !fast && SDL_GetQueuedAudioSize(audio) < FRAME_BYTES * 8)
        SDL_QueueAudio(audio, gg.audio, (Uint32)gg.audio_samples * 4);
    present();
    {   /* test hook: MMPR_SHOT=frame:file.bmp saves the presented screen and exits */
        static int shot_frame = -2; static char shot_file[256];
        if (shot_frame == -2) {
            const char *e = getenv("MMPR_SHOT");
            shot_frame = -1;
            if (e && sscanf(e, "%d:%255s", &shot_frame, shot_file) != 2) shot_frame = -1;
        }
        if (shot_frame >= 0 && (int)gg.frame >= shot_frame) {
            int w, h;
            SDL_GetRendererOutputSize(ren, &w, &h);
            SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
            SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, sf->pixels, sf->pitch);
            SDL_SaveBMP(sf, shot_file);
            quit_now();
        }
    }
    {   /* test hook: MMPR_KEYS="frame:Key,frame:Key" injects key presses */
        const char *e = getenv("MMPR_KEYS");
        static int fired;            /* each event fires once, even if frames repeat after a load */
        for (int idx = 0; e && *e; idx++) {
            int fr = atoi(e);
            const char *c = strchr(e, ':');
            if (!c) break;
            char name[32]; int n = 0;
            for (c++; *c && *c != ',' && n < 31; c++) name[n++] = *c;
            name[n] = 0;
            if (fr == (int)gg.frame && idx >= fired) {
                fired = idx + 1;
                SDL_Event ev; memset(&ev, 0, sizeof ev);
                ev.type = SDL_KEYDOWN; ev.key.keysym.sym = SDL_GetKeyFromName(name);
                SDL_PushEvent(&ev);
            }
            e = *c ? c + 1 : c;
        }
    }
    poll_events();
    while (paused) {
        SDL_Delay(16);
        toast_frames = 2;           /* keep "PAUSED" on screen */
        present();
        poll_events();
        next_tick = (double)SDL_GetPerformanceCounter();
    }
    read_input();
    pace(fast);
}

int main(int argc, char **argv)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    char *bp = SDL_GetBasePath();
    snprintf(base_dir, sizeof base_dir, "%s", bp ? bp : "");
    SDL_free(bp);

    int interp = 0;
    const char *rom_arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--interp")) interp = 1;
        else rom_arg = argv[i];
    }
    int rc = gg_load_rom(find_rom(rom_arg));
    if (rc < 0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "MMPR The Movie (GG)",
            "Could not load the Game Gear ROM.\n\nPlace\n" ROM_NAME "\nnext to this program, or drag the .gg file onto the exe.", NULL);
        return 1;
    }
    if (rc == 1)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "MMPR The Movie (GG)",
            "ROM loaded, but its CRC32 does not match the expected dump (B47C19E5).\n"
            "The recompiled code was generated from that dump; running in interpreter mode.", NULL);
    load_settings();
    if (getenv("MMPR_FILTER")) filter = atoi(getenv("MMPR_FILTER")) % FILTER_COUNT;

    win = SDL_CreateWindow(TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           GG_SCREEN_W * SCALE, GG_SCREEN_H * SCALE, SDL_WINDOW_RESIZABLE);
    SDL_Surface *icon = SDL_CreateRGBSurfaceWithFormatFrom((void *)icon_rgba, 64, 64, 32, 64 * 4, SDL_PIXELFORMAT_ABGR8888);
    if (icon) { SDL_SetWindowIcon(win, icon); SDL_FreeSurface(icon); }
    if (fullscreen) SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    SDL_RenderSetLogicalSize(ren, GG_SCREEN_W, GG_SCREEN_H);
    SDL_RenderSetIntegerScale(ren, SDL_TRUE);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, GG_SCREEN_W, GG_SCREEN_H);
    tex4 = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, FW, FH);
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);
    SDL_SetTextureScaleMode(tex4, SDL_ScaleModeLinear);

    SDL_AudioSpec want = {0}, have;
    want.freq = GG_AUDIO_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 512;
    audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio) SDL_PauseAudioDevice(audio, 0);

    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i) && !pad) pad = SDL_GameControllerOpen(i);

    perf_freq = SDL_GetPerformanceFrequency();
    frame_ticks = (double)perf_freq / FRAME_HZ;
    z80_init();
    gg.frame_callback = host_frame;
    gg.recomp = (uint8_t)(!interp && rc == 0);
    update_title();

    int jump = setjmp(reset_jmp);   /* F2 (reset) and F7 (load state) land here */
    if (audio) SDL_ClearQueuedAudio(audio);
    next_tick = (double)SDL_GetPerformanceCounter();
    if (jump == 2) {
        gg_state_apply();
        update_title();
        gg_boot_resume();           /* never returns */
    }
    gg_reset();
    gg_boot();                      /* runs the game; never returns */
    return 0;
}
