# ROM analysis — Mighty Morphin Power Rangers: The Movie (Game Gear)

## The dump

| | |
|---|---|
| File | `Mighty Morphin Power Rangers - The Movie (USA, Europe, Brazil) (En).gg` |
| Size | 524,288 bytes (512 KB) = 32 banks of 16 KB |
| CRC32 | `B47C19E5` |
| MD5 | `f9acd8f01caa8d11150a53b37b435aa2` |
| SHA-1 | `dfe7dc7b2df143da4f4eeb32051a6d373dddba5d` |
| Header ($7FF0) | `TMR SEGA`, product 2564, version 0, region 7 (GG international), size code 0, checksum field $0000 (GG BIOS doesn't check it) |
| Mapper | Standard Sega mapper, no cartridge RAM (no battery save) |

The `.gg` file is already a raw ROM image — there's no archive layer. "Extracting" means cutting it into banks and disassembling them.

## Memory map, as the game sets it up

Reset code at `$0070` sets SP to `$C0FF`, writes the mapper registers `$FFFC=0, $FFFD=0, $FFFE=1, $FFFF=2`, and clears RAM `$C000-$DFEF`.

| CPU range | Contents |
|---|---|
| `$0000-$3FFF` | bank 0, fixed (kernel, main loop, object system) |
| `$4000-$7FFF` | bank 1, fixed (game logic, IRQ/NMI handlers) |
| `$8000-$BFFF` | slot 2, switched through writes to `$FFFF` (code banks 2, 4, 5, F; the rest is data) |
| `$C000-$DFFF` | 8 KB work RAM (`$DFFF` mirrors the slot-2 bank register, which the IRQ handler reads to save and restore the bank) |

The game never rewrites `$FFFD`/`$FFFE` after reset, so only slot 2 changes at run time.

## Program structure

* **Vectors.** `$0000` reset leads to `$0070`. `$0038` is the IRQ: if the VDP status reports a frame interrupt, it saves all registers and calls the VBlank handler at `$032C`; otherwise it jumps to the line-interrupt handler at `$5683`. `$0066` is NMI → `$4C7E`. On a real Game Gear NMI isn't wired up, so this is probably SMS-heritage pause code.
* **RST helpers.**
  * `rst 08h`: memset(hl, a, bc)
  * `rst 10h`: de = word[hl++]
  * `rst 18h`: set the VDP address to HL
  * `rst 20h`: a = byte[hl + a]
  * `rst 28h`: hl = word[hl + 2·a] (jump/pointer-table fetch)
  * `$0016` (`rst 28h; jp (hl)`): the jump-table dispatcher, normally used as `ld hl,TABLE / call $0016`
* **Game-state machine** (`$0095`). `RAM $C410` holds the current state index. Each frame the loop reads a 4-byte entry from the state table at `$1D23` (`type, bank, entry_lo, entry_hi`; 48 entries), selects that bank in slot 2 and calls the entry. Type 1 runs a pre-step at `$043C`/`$04A6`, and type 2 runs `$04B3`. `$C413` holds a requested next state.
* **Frame sync.** Game code writes a VBlank task number to `$C4C1` and spins at `$03BC` until the IRQ clears it. The VBlank handler dispatches on `$C4C1` through the table at `$0346` (9 tasks), calls the sound driver every frame (`$045F`: bank F, `jp $8000`), then restores the bank. **The PC port syncs to frames at this point:** the spin loop is where the host runs one video frame.
* **Kernel API** at `$0200-$0299`: 52 `jp` stubs that banked modules (mainly banks 4 and 5) call into. All are seeded as functions.
* **Sound driver**: bank F at `$8000`, called once per frame. Music commands `$E0-$FF` dispatch through the table at `$8784`. Commands `$EF`/`$F1` point at a data table (`$8981`) and are never issued; they're recorded in `tools/hints.json` as not-code.
* **Graphics decoder**: `$08xx-$0Bxx` in bank 0, an opcode-driven tile decompressor that dispatches through `$0925` (`jp (hl)` at `$0919`).
* **Object/script interpreter**: `$0E89` dispatches 55 opcodes through `$0EB1`.
* **Input**: port `$00` bit 7 = START, port `$DC` = D-pad + buttons 1/2. Read at `$04C1` and `$4EC9`, among others.

## Disassembly status

`tools/trace.py` does a recursive-descent, bank-aware trace from the vectors, the 48 state entries, the kernel API, and every jump table it can prove (`ld hl,T / rst 28h / jp (hl)` and `ld hl,T / call $0016`), plus the hand-confirmed entries in `tools/hints.json`.

| Bank | Traced code bytes |
|---|---|
| 00 | 14,081 |
| 01 | 12,293 |
| 02 | 4,649 |
| 04 | 282 |
| 05 | 755 |
| 0F | 2,183 (sound driver) |

That's 16,460 instructions, 321 functions and 16 jump tables. Every other bank is data (graphics, maps, music) as far as static analysis can see.

**Round-trip check:** all 32 `disasm/bankNN.asm` files reassemble with `z80asm` to byte-identical copies of the ROM.

**Known gaps** (the runtime miss log is designed to close these):
* Code reached only through pointers stored in object RAM (actor behaviours), and the `jp (hl)` sites at `$1652`, `$20D3` and `$30CC`, whose tables are traced but whose callers' callers may not be.
* Banks 4 and 5 (title, menus, cutscenes) are thinly traced. They lean on the kernel API and their own indirect calls.

## Runtime behaviour (found while bringing up the recompiler)

* **Nested interrupts.** The VBlank handler re-enables interrupts (`ei` at `$0339`) before calling the sound driver, so line interrupts fire inside it. The line handler at `$5683` ends `ei; reti`.
* **Line-interrupt raster effects.** `$5683` walks a table at `$C4D1` and rewrites VDP register 8 (horizontal scroll) and register 10 (line counter) on each interrupt. This produces the split-scroll HUD and parallax.
* **Push-and-jump dispatch in the sound driver.** At `$876C` the sound driver pushes `$8780` and then does `jp (hl)` into its command table, so every handler's `ret` comes back to `$8780`. Some paths also pop a return address to unwind two levels at once.
* **Code in RAM.** `$02DA` builds 256 × `OUTI` followed by `RET` at `$DD00-$DF00`. The game calls into it at different offsets for VRAM transfers of different lengths (e.g. `$0C84`). The runtime runs this as a native routine.
* **RNG.** `ld a,r` at `$0DE2` reads the refresh register. The recompiled code increments R exactly as the CPU does, so randomness matches the interpreter bit for bit.
* **Timing.** One frame is 262 lines × 228 T-states (59.92 Hz). The V counter jumps from `$DA` to `$D5` (NTSC).

## Player data / cheats

| Address | Meaning |
|---|---|
| `$C939` | Player 1 health (240 = full). `0` = round lost (checked at `$478F`) |
| `$C9B9` | Opponent health |
| `$C824` / `$C825` | Health-bar values the HUD draws for P1 / P2 (they follow `$C939` / `$C9B9`) |

The player structures sit at `$C900` and `$C980` (`$80` bytes each). Health is at offset `$39`.

**Infinite health (F1)** blocks any write that would lower `$C939`. It's applied in the memory write path, so it works the same in recompiled and interpreted code and in both Ranger and Megazord fights. With it on, scripted button-mashing clears Stage 1 and reaches Stage 3.
