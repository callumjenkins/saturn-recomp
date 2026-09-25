# saturnkit

A game-agnostic toolkit for Sega Saturn reverse engineering and native PC
ports: disc images, the SH-2 CPU, the hardware's address map, and (to come)
a static recompiler from SH-2 code to C++ and a runtime that replaces the
Saturn's hardware under the recompiled game.

The same idea as [wiikit](https://github.com/vs-sr-dev/wiikit) and
[ps2kit](https://github.com/vs-sr-dev/pc-extermination/tree/main/ps2kit),
for the Saturn. Each Saturn game has its own engine and formats, but a
large part of every port is the *same* work: the same disc layout, the same
two SH-2s, the same VDP1, VDP2, SCU, SMPC, SCSP and CD block, and very
often the same Sega libraries (SBL, SGL). saturnkit collects that shared
part. It grows inside the ports: each piece is written because a game
needed it, then kept free of that game's knowledge. Game formats and game
fixes live in the ports.

## Ports built on it

| Port | Game | What it asked of saturnkit |
|---|---|---|
| pc-virtualhydlide | Virtual Hydlide (1995) | the disc, the SH-2 decoder, the address map |

## Using it

A port takes saturnkit as a git submodule at `saturnkit/`, so that
`python -m saturnkit.…` works from the port's root:

```sh
git submodule add <url>/saturnkit.git saturnkit
git clone --recursive <port>          # or: git submodule update --init
```

Each port pins a saturnkit commit and moves it forward deliberately.

```sh
python -m saturnkit.disc GAME.cue --info              # IP.BIN, ISO 9660, tracks
python -m saturnkit.disc GAME.cue --list
python -m saturnkit.disc GAME.cue --extract build/extract   # files + IP.BIN
python -m saturnkit.disc GAME.cue --audio build/audio       # CD-DA as WAV
python -m saturnkit.sh2 FILE.BIN --find-base               # where it loads
python -m saturnkit.sh2 FILE.BIN --base 0600B000 --at 0600B000 --count 64
python -m saturnkit.sh2 FILE.BIN --base 0600B000 --refs 25D00000:25D00018
python -m saturnkit.sh2 FILE.BIN --base 0600B000 --census
python -m saturnkit.hw 25D00002 25F80114 06000310
```

## Layers

| Layer | Question it answers | Now | Next |
|---|---|---|---|
| 1. Recognise | What is on this disc? | `disc --info`: IP.BIN (product, areas, peripherals, stacks, 1st read), ISO 9660 volume, track list with pregaps | a `fingerprint`: SBL/SGL and their versions by code signature, CRI middleware, sound driver versions |
| 2. Extract | Turn standard formats into standard files | `disc --extract`, `disc --audio` (CD-DA to WAV) | VDP1/VDP2 image decoders (4/8/16 bpp, CLUT, CRAM), Sega FILM/Cinepak, SCSP tone banks |
| 3. Map code | What does the code do, where? | `sh2` (SH7604 decoder, disassembly with literal pools resolved and hardware registers named, `--refs`, `--census`, `--find-base`), `hw` (address map, register names, BIOS service pointers, SCU vectors) | function discovery for SHC/GCC code (prologues, `bsr`/`jsr` literals, `mova`+`braf` switch tables), executable map (crt0, BSS, overlays swapped at the same address), a Python SH-2 interpreter as the recompiler's oracle |
| 4. Translate | Turn SH-2 code into C++ | — | `recomp`: delay slots, T bit, MAC, the `div1` step, one C++ function per entry, per-image dispatch for programs swapped at one address |
| 5. Runtime | Replace the hardware | — | memory map with the cache-through mirrors, SCU (interrupts, DMA), BIOS services, SMPC (pads, INTBACK, slave on/off), the slave SH-2, CD block at its registers (GFS reads, CD-DA), VDP1 on the GPU, VDP2 compositor, 68000 + SCSP; SDL3 window, input and sound |

## Principles

* Pure Python, no dependencies, for layers 1–4; the runtime (layer 5) will
  be C++20 with SDL3, as in wiikit.
* Every claim is checked on a real disc before it goes in.
* Game knowledge stays out.
* Every change is checked on every port before it goes in.

## Checks behind each module

| Module | Checked by |
|---|---|
| `disc` | Virtual Hydlide (Europe), a Redump .cue with 28 tracks: IP.BIN parsed (MK-81380, area E), 458 files extracted from ISO 9660, the 1st read file found (`A.BIN`, identical to the game's own `OPEN.BIN`), track LBAs with their pregaps |
| `sh2` | every one of the 65 536 16-bit words against capstone 5 (SH-2 mode): 53 752 decode to the same mnemonic and operands; the 452 capstone alone decodes are SH-2A extensions (`movua`, `divs`, `jsr/n`, `clips`…) that the SH7604 does not have. `--find-base` puts 14 of 15 Virtual Hydlide executables at the address their crt0 implies |
| `hw` | the register names Virtual Hydlide's code uses, read in context (VDP1 FBCR/PTMR/EDSR, VDP2 TVSTAT and colour offset, SMPC COMREG, FRT FTCSR in the slave's wait loop) |

## Known gaps

* `--find-base` scores literals pointing at function prologues. A small
  program that calls into a large resident one can score higher at a
  wrong base (Virtual Hydlide's MENU.BIN): check against the crt0, whose
  BSS starts where the file ends.
* `hw.BIOS`: the service-pointer names come from the SBL headers; those not
  marked as checked by a game are unconfirmed.
* `sh2.Image.literal_refs` reads every `mov.l @(disp,PC)` in the file,
  data included; a few hits in data areas are noise.

## Licence

MIT — see [LICENSE](LICENSE). saturnkit contains no game data and no Sega
code; it reads and replaces, it does not include.
