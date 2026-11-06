# Paratrooper (1982) — translated to C + SDL2

A port of the original IBM PC game *Paratrooper* (Greg Kuperberg, Orion Software, 1982)
made by **translating the original machine code**, not by rewriting the game.
Every one of the 2,010 instructions in the original `PARA.COM` became a few lines of C,
so the game logic, speed and quirks are exactly the original's.

## How it works

```
original/PARA.COM ──tools/translate.py──► src/game.c   (code, one block per 8086 instruction)
                                       └► src/image.c  (graphics, text and tables from the .COM)
```

| File | What it is |
|---|---|
| `tools/translate.py` | The translator. Follows control flow through the code segment and emits C. |
| `src/game.c` | **Generated.** The original program as C. Don't edit; re-run the translator. |
| `src/cpu.h` | 8086 registers, flags, memory and instruction helpers (`add16`, `shl8`, ...). |
| `src/bios.c` | The bits of BIOS/hardware the game uses: INT 10h/16h/1Ah, PC speaker, game port. |
| `src/main_sdl.c` | SDL2 front end: CGA rendering, keyboard, speaker sound, 4.77 MHz speed control. |
| `src/main_test.c`, `tools/verify.py` | Verification (see below). |

The game only touches the hardware in five ways, and those are the only parts written by hand:
CGA video memory at `B800h`, BIOS keyboard (`INT 16h`), BIOS timer (`INT 1Ah`),
the PC speaker (ports `42h/43h/61h`) and the joystick port (`201h`, reported as "no joystick").

Speed: every translated instruction adds an approximate 8088 cycle count; the front end sleeps
so that emulated time follows real time at 4.77 MHz, and the BIOS timer is derived from it.

## Controls

| Key | Action |
|---|---|
| Left / Keypad 4 | turn gun counter-clockwise |
| Right / Keypad 6 | turn gun clockwise |
| Up / Keypad 8 | stop and fire |
| Space | start game · `I` instructions |
| Ctrl+S | sound on/off (original feature) |
| Esc | abort round (sends Ctrl+Break, like the original) |
| Alt+Enter | fullscreen |

## Building

Windows (prebuilt): run `paratrooper.exe` with `SDL2.dll` next to it.

Linux / MSYS2:
```
make            # needs SDL2 development package (sdl2-config)
make translate  # regenerate src/game.c and src/image.c from original/PARA.COM
```

Windows with Visual Studio: `cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`
then `cmake --build build --config Release`.

## Verification

`tools/verify.py` runs the **original** `PARA.COM` on the Unicorn CPU emulator with the same BIOS
code and the same scripted keypresses as `build/paratrooper_test`. Both print a line per BIOS call
(registers + flags, and every 64th call a CRC of the data segment and video memory):

```
make build/paratrooper_test build/libbios_harness.so
./build/paratrooper_test 400000 > c.txt
python3 tools/verify.py original/PARA.COM build/libbios_harness.so 400000 > ref.txt
cmp c.txt ref.txt
```

Result: identical for 400,000 BIOS calls (title screen, several games, game over, new high score),
including 6,250 memory checksums.

## Note

The original game is copyrighted by Orion Software. `original/PARA.COM` and the generated
`src/image.c`/`src/game.c` are derived from it; keep this for personal study and don't publish
those files.
