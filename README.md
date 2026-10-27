<p align="center"><img src="res/icon_128.png" width="96" alt="icon"></p>

# MMPR: The Movie, Game Gear to PC

A native Windows port of **Mighty Morphin Power Rangers: The Movie** for the Sega Game Gear (1995), made by **static recompilation**. The game's Z80 machine code is translated into C and compiled for x64. A small runtime recreates the Game Gear's video chip, sound chip, controls and bank mapper.

- The **whole game runs as native code**, with zero interpreter fallback, verified by full playthroughs to the ending
- The recompiled code is **bit-identical to a reference Z80 interpreter**: RAM, VRAM, palette and every rendered frame are compared each frame across 400,000+ frames of play
- **Save states**: 10 slots, and a loaded state replays frame-for-frame like the original run
- **Screen filters**: none, LCD (pixel grid and ghosting) or CRT (scanlines and aperture grille)
- Fast-forward, an infinite-health toggle, keyboard and controller support, fullscreen

> **No game data is included.** You need your own ROM of *Mighty Morphin Power Rangers: The Movie (USA, Europe, Brazil) (En)*, CRC32 `B47C19E5`. Release builds look for it next to the exe.

## Playing

Download the zip from **[Releases](../../releases)**, unzip it, put your ROM in the same folder, and run `mmpr_movie_gg.exe`.

| | Keyboard | Controller |
|---|---|---|
| D-pad | Arrows / WASD | D-pad / left stick |
| Button 1 / Button 2 | Z, J / X, K | A, X / B, Y |
| Start | Enter | Start |
| Infinite health | F1 | |
| Reset | F2 | |
| Screen filter (None → LCD → CRT) | F3 | |
| Save state / next slot / load state | F5 / F6 / F7 | |
| Fullscreen | F11 or Alt+Enter | |
| Fast-forward (hold) / Pause / Quit | Tab / P / Esc | |

## Building from source

You need Python 3, CMake, a C compiler and SDL2. On Windows the easiest route is [MSYS2](https://www.msys2.org/) (MinGW64 shell):

```sh
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-SDL2 python

python tools/build_sources.py "path/to/Mighty Morphin Power Rangers - The Movie (USA, Europe, Brazil) (En).gg"
cmake -B build -G "MinGW Makefiles"
cmake --build build
```

`build_sources.py` checks the ROM, writes the disassembly to `disasm/` and writes the recompiled C to `generated/`. Neither folder is committed. On Linux (or WSL) the same steps produce a native build. To cross-compile for Windows, add `-DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DSDL2_DIR=<SDL2-devel-mingw>/x86_64-w64-mingw32/lib/cmake/SDL2`.

## How it works

```
ROM ─ tools/trace.py ─► disasm/ + code_map.json          bank-aware recursive-descent disassembly
    ─ tests/headless -i -c ─► coverage                   interpreter playthroughs log every call/jump target
    ─ tools/recomp.py ─► generated/*.c                   Z80 → C, one function per entry point
                          + runtime/ (VDP, PSG, mapper, scheduler, Z80 interpreter)
                          ─ CMake ─► mmpr_movie_gg.exe
```

- **Code generation.** Each Z80 entry point becomes a C function such as `f_01_5EDA`. Jumps inside it become `goto`, Z80 calls become C calls, jumps to other entry points become tail calls, and computed jumps (`jp (hl)`) go through a dispatch table.
- **Exact timing.** After every instruction the generated code adds the instruction's exact cycle count and runs the same scanline and interrupt check as the interpreter. Line interrupts (used for the split-scroll HUD) therefore land on the same instruction as on the interpreter.
- **Non-standard control flow.** `gg_resume` handles the places where the game doesn't follow normal call/return: the sound driver's push-and-jump command dispatch, returns that skip two levels, and interrupts nested inside the VBlank handler.
- **Code in RAM.** The game builds an unrolled 256× `OUTI` VRAM blaster at `$DD00`, which the runtime runs as a native routine.
- **Save states in a recompiled game.** The machine is captured between two instructions at the end of a frame. Loading `longjmp`s out of the current native call stack and resumes the Z80 code in the interpreter, which jumps back into recompiled functions at the first entry point it reaches.
- **Verification.** `tests/zex_harness.c` runs zexdoc and zexall (67/67). `tests/headless.c` runs the game windowless with scripted input and logs screenshots, WAV audio, per-frame state hashes and coverage. `tests/difftest.sh` compares the recompiled build with the interpreter.

More detail is in [docs/NOTES.md](docs/NOTES.md) (ROM analysis, memory map, RAM addresses) and [docs/ROADMAP.md](docs/ROADMAP.md).

## Legal

This repository contains only original tools, runtime code and documentation (MIT). It contains no ROM, no game code and no game assets. *Mighty Morphin Power Rangers* is a trademark of its respective owners. This project is not affiliated with or endorsed by Saban, Hasbro, Bandai, Banpresto or Sega.
