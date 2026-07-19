/*
 * AmiFetch, a neofetch for AmigaOS
 * by Aigis (P.R_Aigis), 2026
 *
 * My first Amiga program. I have written 3D renderers for 68K Macintoshes
 * before, but the Amiga is new territory for me: custom chips, Kickstart,
 * shared libraries living in ROM, none of which the Mac has. This tool
 * prints system info (CPU, chipset, RAM, Kickstart, expansion boards)
 * next to a procedurally rendered Boing ball, because there is no real
 * neofetch for the Amiga and I own two of them (A1000, A500), so why not.
 *
 * Design goals I set for myself:
 *   - Under 4K after crunching with Shrinkler. This rules out shipping
 *     any image of the ball (a 256x256 bitmap is ~24K raw), so the ball
 *     is generated at runtime from math. Fitting, since the original
 *     1984 Boing demo also computed its ball.
 *   - Runs from CLI, from a Workbench double click, and from a boot
 *     disk, all with the same binary. That means being a polite OS
 *     citizen: open libraries properly, open my own screen through
 *     Intuition, never take over the hardware.
 *   - Works on everything from a Kickstart 1.2 A1000 to a 3.x A4000.
 *     So: no OS calls newer than V33 on the mandatory path, and
 *     anything newer (version.library) is optional and probed.
 *   - No floats and no 32 bit multiply/divide in hot code. The 68000
 *     has no FPU and gcc turns 32 bit muls into a library call
 *     (__mulsi3) that costs about as much as a divide. Everything is
 *     16 bit fixed point and lookup tables. Same discipline as my Mac
 *     Plus demos, the 68000 is the 68000 everywhere.
 *
 * Toolchain: Bartman/Abyss amiga-debug VS Code extension (gcc targeting
 * m68k-amiga-elf, elf2hunk, WinUAE built in).
 *   https://github.com/BartmanAbyss/vscode-amiga-debug
 *
 * References I leaned on, linked again at the relevant spots below:
 *   Amiga Hardware Reference Manual (custom chips, VPOSR etc):
 *     https://amigadev.elowar.com/read/ADCD_2.1/Hardware_Manual_guide/node0000.html
 *   RKM Libraries & Autodocs (exec, graphics, intuition, expansion):
 *     https://amigadev.elowar.com/
 *   English Amiga Board, endless answers to "how do I even":
 *     https://eab.abime.net/
 */

#include "support/gcc8_c_support.h"   /* Bartman's helpers, I use muluw() for
                                         guaranteed single-instruction mulu.w */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <libraries/dos.h>
#include <libraries/dosextens.h>      /* struct Process. I never open
                                         dos.library, but I still need this
                                         struct to tell CLI from Workbench */
#include <libraries/configvars.h>     /* struct ConfigDev for AutoConfig */
#include <libraries/expansionbase.h>
#include <workbench/startup.h>        /* struct WBStartup */
#include <graphics/gfxbase.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>

#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/expansion.h>

/*
 * Library bases. On the Amiga every OS call goes through a library base
 * pointer, and with this toolchain (no startup code doing it for me) I
 * have to define the globals myself. The proto headers declare each base
 * with its "real" struct type, so my definitions must match exactly or
 * gcc complains about conflicting types. Learned that the hard way with
 * ExpansionBase.
 *
 * SysBase is special: exec's base lives at absolute address 4, the only
 * fixed address in the whole OS. Everything else is found through it.
 */
struct ExecBase *SysBase;
struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;
struct ExpansionBase *ExpansionBase;

/* strlen. I have no C library at all (-nostdlib), so I write my own. */
static ULONG slen(const char *s) { const char *p = s; while (*p) ++p; return (ULONG)(p - s); }

/*
 * exec's RawDoFmt() is my printf. It formats into wherever a caller
 * supplied "character output" routine puts each byte. That routine is
 * called with the char in d0 and a user pointer in a3, per the autodoc:
 *   http://amigadev.elowar.com/read/ADCD_2.1/Includes_and_Autodocs_2._guide/node036C.html
 * The canonical minimal routine is two instructions,
 *   move.b d0,(a3)+   ; store char, advance pointer
 *   rts
 * and since I can't write register-calling-convention code in plain C,
 * I just put the two opcodes (0x16C0, 0x4E75) in an array and pass its
 * address as the function. The 68000 does not care that the "function"
 * is const data.
 */
static const UWORD fmtPut[] = { 0x16C0, 0x4E75 };

/*
 * The info lines get collected into this array first and printed later,
 * because layout (next to the ball) needs to know all lines up front.
 * 16 lines of 48 chars is plenty and it is all static, no allocations.
 */
#define MAXINFO 16
static char info[MAXINFO][48];
static UWORD nInfo;

static void addf(const char *fmt, const ULONG *args) {
    if (nInfo < MAXINFO)
        RawDoFmt((STRPTR)fmt, (APTR)args, (void (*)())fmtPut, info[nInfo++]);
}

/*
 * RawDoFmt takes its arguments as a memory block, not varargs on the
 * stack like printf, so this macro gathers the varargs into a ULONG
 * array. The dummy leading 0 exists because ##__VA_ARGS__ with zero
 * arguments would otherwise leave a trailing comma; the array then gets
 * passed starting at element 1. Every argument must be a ULONG because
 * %ld in RawDoFmt reads 32 bits (%d would read 16! the l matters).
 */
#define ADD(fmt, ...) do { const ULONG _a[] = { 0, ##__VA_ARGS__ }; addf(fmt, _a + 1); } while (0)

/* ------------------------------------------------------------------ */
/* Boing ball math                                                     */
/*                                                                     */
/* The ball is a checkered sphere. For every pixel I need latitude and */
/* longitude on the sphere, then the checker cell is just the parity   */
/* of which lat/lon band the pixel falls in. Angles are in my own unit */
/* where 64 = 90 degrees, so a full quarter circle fits a byte and     */
/* band extraction is a shift instead of a divide.                     */
/*                                                                     */
/* Screen trick: the screen is 640 wide hires, where pixels are half   */
/* as wide as they are tall. I draw the ball 256 hires pixels wide and */
/* 128 rows tall, which shows as a circle. The win is that both axes   */
/* map to my internal -254..254 coordinate space with pure shifts.     */
/* ------------------------------------------------------------------ */

#define RR 64516   /* 254 squared. Ball radius in internal units. 254 and
                      not 256 so that r*r stays comfortably below 65536
                      in a few places where I hold values in 16 bits. */

/*
 * asin table: asin8[i] = asin(i/32) scaled so 64 = 90 degrees.
 * 33 entries so index 32 (= input 1.0) is valid. I generated the values
 * with a Python one-liner and typed them in. Why asin: to get longitude
 * from a screen x position you need asin(x / half_width_of_that_row),
 * that is what makes the checker squares visually compress toward the
 * ball's edge. Without the asin the ball looks like a flat UV texture
 * pasted on a circle. I know because my first version did exactly that.
 */
static const UBYTE asin8[33] = {
     0, 1, 3, 4, 5, 6, 8, 9,10,12,13,14,16,17,18,20,
    21,23,24,26,28,29,31,33,35,37,39,41,43,46,50,54,64
};

/* Signed asin lookup. Input is a fraction in -256..256 (256 = 1.0),
   output is an angle in -64..64. The >> 3 turns 0..256 into 0..32
   table steps. Clamp first, the rotated coords can nick past 255. */
static WORD asinS(WORD f) {
    WORD neg = f < 0;
    if (neg) f = -f;
    if (f > 255) f = 255;
    WORD a = asin8[f >> 3];
    return neg ? -a : a;
}

/*
 * Integer square root, This is the bit pair method. It works like long
 * division but in base 4: the probe bit b starts at an even power of
 * two and must shift down by TWO bits per step because square roots
 * consume input bits in pairs. I built this with b >>= 1 at first and
 * spent an evening staring at a "ball" that was 7 pixels wide, because
 * isqrt(64516) came back as 8 instead of 254 and the width table below
 * was garbage. Algorithm reference:
 *   https://en.wikipedia.org/wiki/Integer_square_root
 * Only runs at init time, speed does not matter here. (Debatable)
 */
static UWORD isqrt(ULONG v) {
    ULONG r = 0, b = 1UL << 16;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else            r >>= 1;
        b >>= 2;
    }
    return (UWORD)r;
}

/* ------------------------------------------------------------------ */
/* Precomputed tables. Everything below exists so that the per pixel   */
/* loop in drawBall contains no divides, no function calls and no      */
/* 32 bit multiplies. On a 7 MHz 68000 a divu is ~140 cycles and gcc's */
/* __mulsi3 helper for 32 bit muls costs about the same, and I have    */
/* ~25000 pixels to do. Tables turned a 5 second render into ~1 second.*/
/* ------------------------------------------------------------------ */

static UBYTE wTab[256];    /* wTab[|y|] = half width of the circle at that
                              height, i.e. sqrt(R*R - y*y). Gives me the
                              exact pixel span of every row (so the ball
                              is round by construction, no per pixel
                              inside/outside test) and doubles as the
                              foreshortening divisor for longitude. */
static UWORD recip[256];   /* recip[w] = 65535/w. Turns the per pixel
                              divide g/w into a multiply by reciprocal:
                              (g * recip[w]) >> 8 approximates (g<<8)/w.
                              mulu.w is ~70 cycles vs ~140 for divu.w. */
static UBYTE latB[512];    /* latB[ry+256] = latitude checker index for a
                              rotated y in -256..255, PRE MULTIPLIED by 4
                              (the << 2 in init). Why: see pen formula in
                              drawBall. Offset by 256 because C arrays
                              can't have negative indices. */
static UBYTE lonB[257];    /* lonB[q] = longitude sub band for a positive
                              fraction q = 0..256. */
static UBYTE lonBN[257];   /* Same for negative longitude. Two tables so
                              the inner loop picks one instead of doing
                              sign fixups on the angle. */

static void initWTab(void) {
    for (UWORD i = 0; i < 256; i++) {
        /* R*R - i*i. The (LONG)(WORD)i cast dance forces gcc to emit a
           single muls.w with a 32 bit result (16x16->32 is native on
           the 68000). Without the casts this would be an int*int
           multiply that can overflow or, if done as longs, becomes a
           __mulsi3 library call. v can go negative for i > 254, hence
           the signed LONG and the > 0 test. */
        LONG v = RR - (LONG)(WORD)i * (WORD)i;
        wTab[i] = v > 0 ? (UBYTE)isqrt((ULONG)v) : 0;

        /* 0xFFFFUL is 65535 as an unsigned long, i.e. the biggest value
           whose quotient by any i >= 1 still fits a UWORD. The UL
           suffix keeps the division in 32 bits. Skip i = 0, dividing by
           zero at init would be a great start. */
        if (i) recip[i] = (UWORD)(0xFFFFUL / i);
    }

    /* Latitude bands. asinS gives -64..64, +64 shifts to 0..128, >> 4
       cuts that into 8 bands of 16 units (8 checker rows across the
       sphere, like the real Boing ball). The << 2 premultiplies by 4
       for the pen formula, see drawBall. */
    for (WORD i = -256; i < 256; i++)
        latB[i + 256] = (UBYTE)(((asinS(i) + 64) >> 4) << 2);

    /* Longitude SUB bands: >> 2 instead of >> 4, so 4 sub bands per
       checker cell. The palette cycling rotation steps the ball by one
       sub band, a quarter of a cell, which is what makes the spin look
       smooth instead of snapping cell to cell. */
    for (UWORD q = 0; q <= 256; q++) {
        WORD a = asinS((WORD)q);
        lonB[q]  = (UBYTE)((64 + a) >> 2);
        lonBN[q] = (UBYTE)((64 - a) >> 2);
    }
}

/* ------------------------------------------------------------------ */
/* Plotting                                                            */
/* ------------------------------------------------------------------ */

/*
 * Raw bitplane pointers into the screen's bitmap. Amiga screens are
 * planar: 4 bitplanes means each pixel's 4 bit color number is spread
 * across 4 separate bit arrays. Writing pixels through the OS
 * (WritePixel) is a function call per pixel and is cope on a 68000,
 * so drawBall writes bytes into the planes directly. Poking a screen's
 * bitmap memory myself is fine as long as nothing else draws to it,
 * and it is my own custom screen, so nothing does.
 */
static PLANEPTR plane0, plane1, plane2, plane3;
static UWORD bpr;          /* BytesPerRow of the bitmap */

/*
 * The checkerboard tilt. The iconic Boing ball has its grid tilted
 * roughly 23 degrees off vertical, which is half of what makes it read
 * as a 3D ball at all (my first, untilted version looked like rings).
 * I rotate each pixel's coordinates BEFORE the sphere math, standard
 * 2D rotation with fixed point constants scaled by 256:
 *   cos 23 * 256 = 236, sin 23 * 256 = 100.
 */
#define TILT_C 236
#define TILT_S 100

/*
 * Render the ball (and its shadow) into the bitplanes.
 *
 * Overall shape: for each row, wTab gives the exact span, so I only
 * touch pixels that are on the ball. Per row there are two passes:
 *
 *   pass 1 computes the pen number per pixel and packs the pen's three
 *   low bits into three byte buffers (rb0/rb1/rb2) in fast RAM,
 *
 *   pass 2 copies those buffers into bitplanes 0..2 with one masked
 *   read modify write per byte per plane, and sets plane 3 across the
 *   whole span.
 *
 * Why two passes: chip RAM is slow (the display DMA competes with the
 * CPU for it, especially in hires) and a naive plot() does a read and
 * a write per plane per PIXEL. Packing bits in a buffer first means
 * chip RAM gets touched once per BYTE per plane. This plus the tables
 * plus the reciprocal multiply is the difference between the 5 second
 * render I started with and the ~1 second one.
 *
 * Pen scheme (this is the palette cycling setup): every ball pixel has
 * bit 3 set (plane 3), so ball pens are 8..15. The low 3 bits are
 *   pen = (4 * latChecker + lonSubBand) & 7
 * where latB already stores 4 * latChecker. Why this works: modulo 8,
 * adding 4 flips which half of the 8 pen palette you are in, so
 * adjacent latitude bands automatically show opposite colors (that is
 * the checkerboard), while the low two bits advance with longitude in
 * quarter cell steps. Rotating the 8 palette entries by one therefore
 * rotates the ball by a quarter of a checker cell. The original 1984
 * Boing demo animated its ball exactly this way, color registers only:
 *   https://amiga.lychesis.net/applications/AmigaBoingBall.html
 *
 * The toggle: lat/lon only gets recomputed every second pixel. Hires
 * pixels are half width, so doubling each sample horizontally just
 * makes the ball effectively lowres sharp, which the original was
 * anyway. Halves the math for free.
 */
static void drawBall(struct RastPort *rp, WORD cx, WORD cy) {
    static UBYTE rb0[80], rb1[80], rb2[80];  /* one row of pen bits, 80
                                                bytes covers the full 640
                                                pixel screen width */

    /* Shadow first, ball overwrites the overlap. Drawn with RectFill
       (one OS call per row) instead of pixel pokes since it is just a
       flat ellipse of pen 3, offset right and down from the ball. The
       original demo had a shadow too, it sells the 3D. */
    SetAPen(rp, 3);
    for (WORD dy = -64; dy < 64; dy++) {
        /* dy is the screen row, -64..63. fy maps it to internal units:
           << 2 because 128 rows must span 512 units (rows are 4 units
           apart, hires pixels 2), +2 samples the row's center. */
        WORD fy = (dy << 2) + 2;
        UWORD w = wTab[fy < 0 ? -fy : fy];
        if (!w) continue;
        WORD dxm = (WORD)((w - 1) >> 1);  /* half span in PIXELS: w is in
                                             internal units (2 per hires
                                             pixel), so divide by 2 */
        RectFill(rp, cx + 20 - dxm, cy + 10 + dy, cx + 20 + dxm, cy + 10 + dy);
    }

    for (WORD dy = -64; dy < 64; dy++) {
        WORD fy = (dy << 2) + 2;
        UWORD w = wTab[fy < 0 ? -fy : fy];
        if (!w) continue;
        WORD dxm = (WORD)((w - 1) >> 1);
        WORD x0 = cx - dxm, x1 = cx + dxm;   /* pixel span of this row */
        UWORD b0 = x0 >> 3, b1 = x1 >> 3;    /* same span in bytes */

        /* Tilt rotation, set up incrementally. rxA/ryA hold the rotated
           coordinates in 24.8 fixed point (hence LONG). Instead of two
           multiplies per pixel, I compute the rotated coords of the
           row's FIRST pixel here, and inside the loop each step just
           adds the per pixel delta (2*TILT_C, 2*TILT_S), because
           rotation is linear. Same incremental trick I used for
           scanline interpolation in my Mac renderer.
           -(dxm << 1) + 1 is the first pixel's x in internal units
           (2 units per pixel, +1 for the pixel center). */
        LONG rxA = (LONG)(WORD)(-(dxm << 1) + 1) * TILT_C - (LONG)fy * TILT_S;
        LONG ryA = (LONG)(WORD)(-(dxm << 1) + 1) * TILT_S + (LONG)fy * TILT_C;

        /* Clear just the buffer bytes this row will touch. */
        for (UWORD i = b0; i <= b1; i++) rb0[i] = rb1[i] = rb2[i] = 0;
        UBYTE *q0 = &rb0[b0], *q1 = &rb1[b0], *q2 = &rb2[b0];
        UBYTE bit = 0x80 >> (x0 & 7);  /* bitplanes are MSB first: pixel
                                          x=0 is bit 7 of byte 0 */
        UBYTE a0 = 0, a1 = 0, a2 = 0;  /* accumulators for the byte being
                                          built, one per plane */

        UWORD pen = 0, toggle = 0;
        for (WORD dx = -dxm; dx <= dxm; dx++, rxA += 2 * TILT_C, ryA += 2 * TILT_S) {
            if (!toggle) {
                /* Back from 24.8 fixed point to integer units. */
                WORD rx = (WORD)(rxA >> 8);
                WORD ry = (WORD)(ryA >> 8);

                /* Longitude. g is |rx|, ww the sphere's half width at
                   this ROTATED latitude, and q = (g << 8) / ww is the
                   0..256 fraction whose asin is the longitude. The
                   divide is done as a multiply by recip[ww]. At the
                   very rim ww can be 0 or g can exceed ww from fixed
                   point rounding, both clamp to q = 256 (the pole). */
                UWORD ay = ry < 0 ? (UWORD)-ry : (UWORD)ry;
                UWORD ww = wTab[ay];
                UWORD g  = rx < 0 ? (UWORD)-rx : (UWORD)rx;
                UWORD q;
                if (!ww || g >= ww) q = 256;
                else                q = (UWORD)(muluw(g, recip[ww]) >> 8);

                /* The pen formula described above. latB is already
                   4 * latitude band. & 7 keeps the low 3 bits, plane 3
                   is added implicitly in pass 2. */
                pen = (UWORD)(latB[ry + 256] + (rx < 0 ? lonBN[q] : lonB[q])) & 7;
            }
            toggle ^= 1;

            /* Spread the pen's bits into the three plane accumulators. */
            if (pen & 1) a0 |= bit;
            if (pen & 2) a1 |= bit;
            if (pen & 4) a2 |= bit;
            bit >>= 1;
            if (!bit) { *q0++ |= a0; *q1++ |= a1; *q2++ |= a2;
                        a0 = a1 = a2 = 0; bit = 0x80; }
        }
        /* Flush a partial final byte (span rarely ends on a byte edge). */
        if (bit != 0x80) { *q0 |= a0; *q1 |= a1; *q2 |= a2; }

        /* Pass 2: masked copy into chip RAM. lm/rm trim the first and
           last byte so pixels outside the span survive (the grid drawn
           under the ball). The (ULONG)(UWORD) cast on the row keeps the
           offset math as a single mulu.w again. Plane 3 just gets the
           mask ORed in: every ball pixel sets bit 3, putting all ball
           pens at 8..15 so they can be cycled without touching the four
           UI colors in pens 0..3. */
        ULONG off = (ULONG)(UWORD)(cy + dy) * bpr + b0;
        UBYTE *p0 = plane0 + off, *p1 = plane1 + off;
        UBYTE *p2 = plane2 + off, *p3 = plane3 + off;
        UBYTE lm = 0xFF >> (x0 & 7);
        UBYTE rm = (UBYTE)(0xFF << (7 - (x1 & 7)));
        for (UWORD i = b0; i <= b1; i++, p0++, p1++, p2++, p3++) {
            UBYTE m = 0xFF;
            if (i == b0) m &= lm;
            if (i == b1) m &= rm;
            *p0 = (UBYTE)((*p0 & ~m) | (rb0[i] & m));
            *p1 = (UBYTE)((*p1 & ~m) | (rb1[i] & m));
            *p2 = (UBYTE)((*p2 & ~m) | (rb2[i] & m));
            *p3 = (UBYTE)(*p3 | m);
        }
    }
}

/* ------------------------------------------------------------------ */
/* System info                                                         */
/* ------------------------------------------------------------------ */

/*
 * Gayle ID probe. Gayle is the A600/A1200 gate array and it has a fun
 * identification protocol: writing anything to $DE1000 resets a serial
 * shift register, then each read hands you one ID bit in bit 7, MSB
 * first, and a write advances to the next bit. Eight reads assemble
 * the ID byte, 0xD0 meaning A600/A1200. On machines without Gayle the
 * reads hit open bus and won't produce a consistent 0xD0. Documented
 * mostly in driver source and forum archaeology, e.g.:
 *   https://eab.abime.net/
 * and the Linux/m68k and AROS Gayle detection code.
 */
static UBYTE gayleId(void) {
    volatile UBYTE *g = (volatile UBYTE *)0xDE1000;
    UBYTE id = 0;
    *g = 0;
    for (UWORD i = 0; i < 8; i++) {
        id = (UBYTE)((id << 1) | ((*g & 0x80) ? 1 : 0));
        *g = 0x80;
    }
    return id;
}

/*
 * Model detection, the "hostname". Disclaimer: the Amiga has NO model 
 * register anywhere. Every program that prints "A1200" is guessing 
 * from which support chips answer. My heuristics:
 *
 *   Ramsey revision register at $DE0043: 0x0D on an A3000, 0x0F on an
 *   A4000 (Ramsey is their memory controller, other models read open
 *   bus here).
 *   Akiko at $B80002 reads 0xCAFE (lol) on a CD32.
 *   Gayle ID 0xD0 plus a 68000 means A600 (an A1200 has Gayle too but
 *   also a 68020 and AGA, so it is caught earlier).
 *   Chipset class (AGA/ECS/OCS) from the Agnus ID narrows the rest.
 *   A500 trapdoor "slow" RAM living at $C00000 is sign of an A500.
 *   I walk exec's memory list looking for a region there.
 *
 * All probes are reads (plus Gayle's protocol writes, which hit open
 * bus  on machines without Gayle). On a 68000 unmapped
 * addresses just read floating bus, no bus error.
 *
 * A base OCS machine with no trapdoor RAM genuinely cannot be narrowed
 * further, so my own A1000 reports "Amiga 500/1000/2000". I would
 * rather print a truthful range than a confident LARP LARP LARP SAHUR.
 */
static const char *detectModel(UWORD att, UWORD agnusId) {
    UWORD aga = (agnusId & 0x20) && (agnusId & 0x02);
    UWORD ecs = (agnusId & 0x20) && !aga;
    UBYTE ramsey = *(volatile UBYTE *)0xDE0043;

    if (aga) {
        if (*(volatile UWORD *)0xB80002 == 0xCAFE) return "CD32";
        if (ramsey == 0x0F) return "Amiga 4000";
        return "Amiga 1200";
    }
    if (ecs) {
        if (ramsey == 0x0D) return "Amiga 3000";
        if (!(att & AFF_68020) && gayleId() == 0xD0) return "Amiga 600";
        return "Amiga 500+/2000";
    }
    Forbid();   /* walking a system list, so freeze task switching */
    for (struct Node *n = SysBase->MemList.lh_Head; n->ln_Succ; n = n->ln_Succ) {
        struct MemHeader *mh = (struct MemHeader *)n;
        if ((ULONG)mh->mh_Lower >= 0xC00000 && (ULONG)mh->mh_Lower < 0xD80000) {
            Permit();
            return "Amiga 500";
        }
    }
    Permit();
    return "Amiga 500/1000/2000";
}

/*
 * Kickstart marketing name from exec's version number. Version numbers
 * are the truth on the Amiga ("V36" etc in all the docs), the names
 * are for humans. Table assembled from:
 *   https://en.wikipedia.org/wiki/Kickstart_(Amiga)
 * I dropped V30-32 (Kickstart 1.0/1.1) support from the whole program,
 * nobody runs them, even A1000 owners boot 1.2 or 1.3 disks.
 * Def not cope for me not writing it lol :3
 */
static const char *kickName(UWORD v) {
    switch (v) {
        case 33: return "1.2";  case 34: case 35: return "1.3";
        case 36: return "2.0";  case 37: return "2.04";
        case 38: return "2.1";  case 39: return "3.0";
        case 40: case 43: return "3.1";
        case 44: return "3.5";  case 45: return "3.9";
        case 46: return "3.1.4"; case 47: return "3.2";
    }
    return "?";
}

/*
 * Gather all the info lines. Everything here is read from exec's base
 * structure or via official OS calls, no hardware banging except the
 * two documented read only ID probes (Agnus, and detectModel's chips).
 */
static void collectInfo(void) {
    /* CPU/FPU from exec's AttnFlags, a bitfield exec fills at boot by
       actually testing instructions:
         http://amigadev.elowar.com/read/ADCD_2.1/Includes_and_Autodocs_2._guide/node009E.html
       Checked highest first since an 040 sets the 030 and 020 bits too.
       The 68060 flag is bit 7 but AFF_68060 is missing from these old
       NDK headers, so it is a raw (1 << 7). AFF_FPU40 means a 68040/060
       with the FPU on the die rather than a separate 6888x. */
    UWORD att = SysBase->AttnFlags;
    const char *cpu = "MC68000";
    if      (att & (1 << 7))   cpu = "MC68060";
    else if (att & AFF_68040)  cpu = "MC68040";
    else if (att & AFF_68030)  cpu = "MC68030";
    else if (att & AFF_68020)  cpu = "MC68020";
    else if (att & AFF_68010)  cpu = "MC68010";
    const char *fpu = "none";
    if      (att & AFF_FPU40)  fpu = "internal";
    else if (att & AFF_68882)  fpu = "MC68882";
    else if (att & AFF_68881)  fpu = "MC68881";

    /* Chipset from the Agnus ID in bits 8-14 of VPOSR ($DFF004). It is
       a read only register so peeking it under the OS is fine.
       Bit 5 set means ECS or later, and within that, bit 1 set means
       Alice (AGA). Hardware Reference Manual, custom register section:
         https://amigadev.elowar.com/read/ADCD_2.1/Hardware_Manual_guide/node0000.html */
    UWORD agnusId = (*(volatile UWORD *)0xDFF004 >> 8) & 0x7F;
    const char *chipset = "OCS";
    if (agnusId & 0x20) chipset = (agnusId & 0x02) ? "AGA" : "ECS";

    /* Kickstart version is just exec.library's own version field.
       Workbench's version comes from version.library, which only
       exists on 2.0+ (V36), so it is opened with version 0 = "any"
       and quietly skipped when absent. On my 1.3 machines there is
       simply no Workbench line, which is correct. */
    UWORD kv = SysBase->LibNode.lib_Version;
    UWORD kr = SysBase->LibNode.lib_Revision;
    UWORD wbv = 0, wbr = 0;
    struct Library *ver = OpenLibrary((STRPTR)"version.library", 0);
    if (ver) { wbv = ver->lib_Version; wbr = ver->lib_Revision; CloseLibrary(ver); }

    /* Memory totals by walking exec's MemList: each MemHeader is one
       memory region, size = upper bound minus lower bound, sorted into
       chip vs everything else by the MEMF_CHIP attribute. System lists
       can change under you, so the walk sits inside Forbid()/Permit()
       (no task switches while I hold the list). Free amounts come from
       AvailMem(). Fun quirk: A500 trapdoor "slow" RAM counts as
       non chip here because that is what exec calls it, even though it
       is just as slow as chip RAM. Technically correct. */
    ULONG chipTot = 0, fastTot = 0;
    Forbid();
    for (struct Node *n = SysBase->MemList.lh_Head; n->ln_Succ; n = n->ln_Succ) {
        struct MemHeader *mh = (struct MemHeader *)n;
        ULONG sz = (ULONG)mh->mh_Upper - (ULONG)mh->mh_Lower;
        if (mh->mh_Attributes & MEMF_CHIP) chipTot += sz; else fastTot += sz;
    }
    Permit();
    ULONG chipFree = AvailMem(MEMF_CHIP);
    ULONG fastFree = AvailMem(MEMF_FAST);

    ADD("Host: %s", (ULONG)detectModel(att, agnusId));
    ADD("Kickstart: %s (%ld.%ld)", (ULONG)kickName(kv), (ULONG)kv, (ULONG)kr);
    if (wbv) ADD("Workbench: %ld.%ld", (ULONG)wbv, (ULONG)wbr);
    ADD("CPU: %s   FPU: %s", (ULONG)cpu, (ULONG)fpu);
    /* PAL vs NTSC from exec's VBlankFrequency (50 or 60). */
    ADD("Chipset: %s (%s)", (ULONG)chipset,
        (ULONG)(SysBase->VBlankFrequency == 60 ? "NTSC" : "PAL"));
    /* >> 10 = divide by 1024, sizes shown in K like a proper Amiga. */
    ADD("Chip RAM: %ldK free / %ldK", chipFree >> 10, chipTot >> 10);
    if (fastTot) ADD("Other RAM: %ldK free / %ldK", fastFree >> 10, fastTot >> 10);

    /* Expansion boards via the AutoConfig chain. FindConfigDev with
       -1, -1 wildcards iterates every configured board; each has a
       manufacturer and product number burned into its config ROM.
       Printing the raw numbers costs nothing, a name lookup table is
       a "thicc build" idea for later, bytes permitting.
         http://amigadev.elowar.com/read/ADCD_2.1/Includes_and_Autodocs_2._guide/node05A0.html */
    if (ExpansionBase) {
        struct ConfigDev *cd = NULL;
        while ((cd = FindConfigDev(cd, -1, -1)))
            ADD("Board: Man %ld Prod %ld (%ldK)",
                (ULONG)cd->cd_Rom.er_Manufacturer,
                (ULONG)cd->cd_Rom.er_Product,
                cd->cd_BoardSize >> 10);
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

/* Draw a string with the rastport's default font. Text() in the
   default JAM2 mode fills the whole character cell with background,
   which screwed me once: I tried to underline a heading by printing
   underscores two pixels below it and the second Text() erased the
   first. Underlines are drawn with Draw() now. */
static void textAt(struct RastPort *rp, WORD x, WORD y, const char *s, UWORD pen) {
    SetAPen(rp, pen);
    Move(rp, x, y);
    Text(rp, (STRPTR)s, slen(s));
}

int main() {
    /* Address 4 is AbsExecBase, the one hardwired address in AmigaOS. */
    SysBase = *((struct ExecBase **)4UL);

    /* CLI or Workbench? A process started from Workbench has no CLI
       structure, and Workbench sends it a WBStartup message that MUST
       be collected (WaitPort + GetMsg) before doing much of anything,
       and replied to at exit. This is the documented startup protocol
       that a real startup.o would do for me, but with -nostdlib I am
       my own startup code.
         https://wiki.amigaos.net/wiki/Workbench_Library */
    struct Process *pr = (struct Process *)FindTask(NULL);
    struct WBStartup *wbmsg = NULL;
    if (!pr->pr_CLI) {
        WaitPort(&pr->pr_MsgPort);
        wbmsg = (struct WBStartup *)GetMsg(&pr->pr_MsgPort);
    }

    /* Version 33 = Kickstart 1.2, my compatibility floor. Expansion is
       optional (the info line just disappears without it), graphics
       and intuition are not, no screen means no program. */
    GfxBase       = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 33);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 33);
    ExpansionBase = (struct ExpansionBase *)OpenLibrary((STRPTR)"expansion.library", 33);
    if (!GfxBase || !IntuitionBase) goto bail;

    collectInfo();
    initWTab();

    /* My own screen instead of a window on Workbench, for one reason:
       the palette. On a stock 1.3 Workbench I would get 4 colors and
       an orange ball. My screen = guaranteed red and white Boing plus
       my 8 cycling pens. NTSC machines only have 200 visible lines, so
       the height follows VBlankFrequency. 4 bitplanes = 16 colors,
       needed for the palette cycled spin (pens 8..15 are the ball).
       SCREENQUIET suppresses Intuition's title bar rendering. Costs
       ~80K of chip RAM at 640 wide, tight on a 256K A1000 but fits. */
    UWORD h = SysBase->VBlankFrequency == 60 ? 200 : 256;
    struct NewScreen ns = {
        0, 0, 640, h, 4,
        0, 1, HIRES,
        CUSTOMSCREEN | SCREENQUIET,
        NULL, NULL, NULL, NULL     /* default font, no title, no gadgets */
    };
    struct Screen *scr = OpenScreen(&ns);
    if (!scr) goto bail;

    struct RastPort *rp = &scr->RastPort;
    struct ViewPort *vp = &scr->ViewPort;

    /* UI palette. SetRGB4 takes 4 bits per gun, the OCS color format
       (12 bit color, same as the hardware color registers). */
    SetRGB4(vp, 0, 0xA, 0xA, 0xA);  /* background, light gray */
    SetRGB4(vp, 1, 0xF, 0xF, 0xF);  /* white */
    SetRGB4(vp, 2, 0xF, 0x0, 0x0);  /* red, heading text */
    SetRGB4(vp, 3, 0x6, 0x6, 0x6);  /* dark gray: grid, shadow, info text */

    /* Ball pens 8..15, initial phase: first four red, last four white.
       Which pens are red rotates later, that IS the animation. */
    for (UWORD i = 0; i < 8; i++)
        SetRGB4(vp, 8 + i, i < 4 ? 0xF : 0xF, i < 4 ? 0x0 : 0xF, i < 4 ? 0x0 : 0xF);

    SetRast(rp, 0);   /* clear to pen 0 */

    /* The purple-less version of the backdrop: a plain grid.
       Drawn first so the ball and shadow sit on top of it. */
    SetAPen(rp, 3);
    for (WORD x = 0; x < 640; x += 32) { Move(rp, x, 0); Draw(rp, x, h - 1); }
    for (WORD y = 0; y < h;   y += 16) { Move(rp, 0, y); Draw(rp, 639, y);  }

    /* Hand the raw bitmap to the renderer and go. */
    plane0 = scr->BitMap.Planes[0];
    plane1 = scr->BitMap.Planes[1];
    plane2 = scr->BitMap.Planes[2];
    plane3 = scr->BitMap.Planes[3];
    bpr    = scr->BitMap.BytesPerRow;
    drawBall(rp, 150, h / 2);

    /* Heading plus a drawn underline (see textAt for why drawn).
       8 * 8 - 1: topaz 8 is 8 pixels per character, 8 characters. */
    textAt(rp, 320, 40, "AmiFetch", 2);
    SetAPen(rp, 2);
    Move(rp, 320, 44); Draw(rp, 320 + 8 * 8 - 1, 44);

    for (UWORD i = 0; i < nInfo; i++)
        textAt(rp, 320, 62 + i * 12, info[i], 3);
    textAt(rp, 8, h - 12, "P.R_Aigis 2026", 3);
    textAt(rp, 420, h - 12, "Click mouse to exit.", 3);

    /* The spin, and the reason the ball can spin at all on a 7 MHz
       68000: nothing is redrawn. The ball was rendered once with 8
       pens laid out in quarter-cell longitude bands, so rotating WHICH
       four of the eight are red shifts the whole texture by a quarter
       cell. Eight SetRGB4 calls per step, the CPU is otherwise idle.
       This is the original Boing demo's trick.

       WaitTOF() (wait for top of frame) paces the loop at 50/60 Hz and
       makes the color changes land in the vertical blank, so the ball
       never shows a half updated palette. Every 6 frames = one step,
       a lazy globe spin, smaller divisor = faster.

       Exit poll: left mouse button is bit 6 of CIA-A port A at
       $BFE001, active low. Reading a CIA port is harmless and works
       on every Kickstart, and it saves me opening a window just to
       get IDCMP mouse events. Hardware manual, CIA appendix:
         https://amigadev.elowar.com/read/ADCD_2.1/Hardware_Manual_guide/node0000.html */
    UWORD tick = 0, shift = 0;
    while (*(volatile UBYTE *)0xBFE001 & 0x40) {
        WaitTOF();
        if (++tick >= 6) {
            tick = 0;
            shift = (shift + 1) & 7;
            for (UWORD i = 0; i < 8; i++) {
                UWORD on = ((i + shift) & 7) < 4;   /* is this pen red now */
                SetRGB4(vp, 8 + i, 0xF, on ? 0x0 : 0xF, on ? 0x0 : 0xF);
            }
        }
    }

    CloseScreen(scr);

bail:
    /* Close in reverse order of opening, skipping whatever never
       opened. CloseLibrary wants a plain struct Library pointer, the
       casts undo the typed base pointers. */
    if (ExpansionBase) CloseLibrary((struct Library *)ExpansionBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    if (GfxBase)       CloseLibrary((struct Library *)GfxBase);

    /* The other half of the Workbench protocol: reply the startup
       message LAST, inside Forbid(). The moment Workbench gets the
       reply it may unload this code from memory, and Forbid() holds
       task switching off until my exit actually completes, so the rug
       cannot be pulled mid-instruction. Exiting drops the Forbid. */
    if (wbmsg) {
        Forbid();
        ReplyMsg((struct Message *)wbmsg);
    }
    return 0;
}
