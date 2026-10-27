# Roadmap

## Done

### 0. Extraction and analysis
- [x] ROM identified, hashed and header decoded
- [x] Mapper usage, vectors, state machine, frame sync and kernel API worked out (NOTES.md)
- [x] Bank-aware tracer; full disassembly of all 32 banks, verified byte-identical by reassembly

### 1. Runtime
- [x] Z80 interpreter: every documented and undocumented opcode. **Passes zexdoc and zexall (67/67)**
- [x] VDP Mode 4 renderer: background, sprites, flips and priority, scroll locks, line interrupts, GG 160×144 viewport, 12-bit colour
- [x] SN76489 PSG: tones and noise, GG stereo, cycle-timestamped register writes
- [x] SDL2 host: window, integer scaling, fullscreen, keyboard and controllers, audio-paced 59.92 Hz

### 2. Static recompiler (`tools/recomp.py`)
- [x] One C function per entry point: gotos within a function, C calls for Z80 calls, tail calls between functions, dispatch for computed jumps
- [x] Exact T-state accounting and the same between-instruction scheduler check as the interpreter, so scanline IRQs land on the same instruction
- [x] Non-standard control flow handled through `gg_resume`: push-and-jump dispatch in the sound driver, returns that skip a frame, interrupts nested inside the IRQ handler
- [x] Entry points from static tracing plus **dynamic coverage**: interpreter playthroughs log every call target and computed jump target
- [x] RAM code: the game's unrolled `OUTI` VRAM blaster at `$DD00` runs as a native routine
- [x] **Differential test:** recompiled and interpreted runs produce bit-identical RAM, VRAM, CRAM and frames, checked every frame across 376,000 frames of scripted play (story mode incl. Megazord fights, VS, link, options, attract) with zero interpreter fallback; the Windows (MinGW) build reproduces the same hashes

### 3. Extras
- [x] F1 infinite health (blocks writes that lower `$C939`); reset moved to F2

### 4. Verification
- [x] Full playthrough to the ending on Windows: no visual or audio issues
- [x] Ending-sequence entries from that run (`$626C`, `$677F`, `$32A5`-`$3349`) added to `tools/hints.json`
- [x] **Second full playthrough: zero `recomp_misses`. The whole game runs as recompiled native code.**

### 5. Release 1.0
- [x] Save states: 10 slots (F5 save, F6 slot, F7 load). Loading reproduces the original run exactly (tested in the headless runner, the Linux host and the Windows build)
- [x] Screen filters (F3): LCD (pixel grid and ghosting) and CRT (scanlines and aperture grille), remembered in `settings.ini`
- [x] App icon (placeholder, replace `res/icon.ico` and `res/icon_1024.png`), version info, player README, SDL2 licence
- [x] `release/MMPR-Movie-GG-PC-v1.0.zip`

## Optional polish
- [ ] Rebindable controls
- [ ] Batch the scheduler checks per basic block for speed (it already runs far faster than 60 fps)
