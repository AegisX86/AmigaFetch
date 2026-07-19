# AmiFetch

A neofetch for AmigaOS. Renders a procedurally generated, spinning Boing
ball next to your machine's system information, on a screen it opens
itself. Under 4K when crunched.

<a href="photos/a1000.png"><img src="photos/a1000.png" alt="AmiFetch on my Amiga 1000" width="500"></a>


Shows: detected model, Kickstart and Workbench versions, CPU and FPU,
chipset (OCS/ECS/AGA) and video system (PAL/NTSC), chip and other RAM
(free/total), and any AutoConfig expansion boards.

The ball is not an image. A 256x256 bitmap would be ~24K on its own,
so the ball is computed at boot from integer math (asin table, integer
square root, fixed point tilt rotation) and the spin is palette cycling,
the same trick the original 1984 Boing demo used. No floats, no 32-bit
multiplies or divides in the hot path. The 68000 in your A500 redraws
nothing while it spins.

## Requirements

- Any Amiga, Kickstart 1.2 (V33) or newer
- ~80K free chip RAM for the screen
- You, the user :3 

## Running it

Grab the release: `AmiFetch` (plain executable) or `AmiFetch.adf`
(bootable disk).

- **From CLI/Shell:** just run `AmiFetch`
- **From Workbench:** double-click the icon
- **As a boot disk:** write the ADF to a floppy (or mount it in an
  emulator) and boot from it. The same binary handles all three.

Click the left mouse button to exit.

## Status / honesty section

I have tested this on my real Amiga 1000, and in WinUAE
against A500, A600, A1200, and A4000 configurations. I do
**not** own most of those machines. Emulation is good but it is not
hardware, so on real ECS/AGA big-box Amigas, accelerated machines, or
unusual configurations there may be bugs or strange behavior. In
particular:

- Model detection is heuristic. The Amiga has no model register, so
  AmiFetch guesses from chip probes (Ramsey, Gayle, Akiko, Agnus ID).
  A stock OCS machine without trapdoor RAM honestly reports
  "Amiga 500/1000/2000" because to my knowledge it cannot be narrowed
  further.
- The Gayle ID probe and Ramsey revision values are folklore-grade
  documentation. If your machine is misdetected, please open an issue
  and tell me what it printed and what it actually is.

## Building

You need [Bartman/Abyss' amiga-debug VS Code extension](https://github.com/BartmanAbyss/vscode-amiga-debug), which bundles the
whole toolchain (gcc for m68k-amiga-elf, elf2hunk, WinUAE/FS-UAE, GDB).
Windows, mac and Linux all should work.

1. Install VS Code and the amiga-debug extension
2. Make an empty folder, open it in VS Code, and run
   `Amiga: Init Project` from the command palette. This drops in the
   toolchain glue (`support/` and friends).
3. Copy this repo's files into that folder, overwriting the template's
   `main.c`, `Makefile`, `.vscode/` and adding `disk/` and the makedisk
   scripts. The `.vscode/settings.json` here sets
   `"amiga.program": "out/AmiFetch"`, which is what names the output;
   if you merge settings by hand instead of copying, keep that line or
   you get the template's `out/a.exe` back.
4. Point the extension at a Kickstart 1.3 ROM (settings, or
   `.vscode/launch.json` under `"kickstart"`). ROMs are copyrighted;
   get them from Cloanto's Amiga Forever or dump your own machine's.
5. Press F5: builds and launches in the bundled emulator

Command line builds work too, via `Amiga: Open Terminal` in the VS Code
command palette, then `gnumake`. Output lands in `out/AmiFetch.exe`
(AmigaDOS executable) plus `out/AmiFetch.elf` (for debugging) and
`out/AmiFetch.s` (disassembly, worth reading if you touch drawBall).

## Release build and making the disk

For a release build, remove `-g` from `CCFLAGS` in the Makefile (debug
info is dead weight and Shrinkler would happily compress it), clean,
and rebuild. Then run the disk script from the project folder:

- **Windows:** `makedisk.cmd`
- **Linux/macOS:** `./makedisk.sh`

The script finds Shrinkler (on PATH, or bundled inside the amiga-debug
extension), crunches `out/AmiFetch.exe` with the same flags as the
extension's "slow" preset (`-h -f dff180 -9`: hunk executable, max
compression, and the background flashes while it decrunches, if you 
don't like this behavior, disable it by removing -f dff180), and builds
`AmiFetch.adf`: OFS format (Kickstart 1.3 cannot boot FFS floppies),
installed bootblock, the crunched exe as `AmiFetch`, the icons, and
`s/startup-sequence`. The result boots on real hardware and also mounts 
on a running Workbench, where the program launches from its icon.

You can also pass a different input exe: `makedisk.cmd out\Whatever.exe`.
The scripts need [amitools](https://github.com/cnvogelg/amitools)
(`pip install amitools`).

You can still crunch manually instead: right-click `out/AmiFetch.exe`
in VS Code and pick Shrinkler ("slow" preset = the flags above, defined
in `.vscode/amiga.json`). The size profiler will show you where the
bytes go.

## Code tour

Everything is in `main.c`, heavily commented,  the comments are my
working notes on why things are the way they are, including the bugs I
hit along the way. Rough map:

- ball math: asin/sqrt tables, why hires pixels make the circle
- `drawBall()`: two-pass row renderer writing bitplanes directly
- `detectModel()` and friends: the sysinfo probes
- `main()`: CLI/Workbench startup protocol, screen setup, the
  palette-cycling spin loop

`support/` is from Bartman's project template and provides the startup
glue and `muluw()` helpers.

## Credits

- Written by Aigis (P.R_Aigis), 2026. My first Amiga program.
- Built with Bartman/Abyss' amiga-debug toolchain
- Executable crunched with [Shrinkler](https://github.com/askeksa/Shrinkler)
  by Blueberry/Loonies
- The Boing ball is by Dale Luck and RJ Mical, 1984.
</markdown>