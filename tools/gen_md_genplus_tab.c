/*
 * ThumbyNES — generator for Genesis Plus GX YM2612 lookup tables
 * (tl_tab, sin_tab, lfo_pm_table) used by ym2612_genplus.c.
 *
 * Upstream GenPlus computes these at first init via init_tables() in
 * ym2612_genplus.c. That costs ~158 KB of BSS (or heap, in our build):
 *
 *   tl_tab        TL_TAB_LEN * sizeof(short)      = 13*2*256*2 = 13.3 KB
 *   sin_tab       SIN_LEN    * sizeof(unsigned)   = 1024*4     =  4   KB
 *   lfo_pm_table  128*8*32   * sizeof(int)        = 32768*4    = 128  KB
 *
 * On the RP2350 device build we precompute these at build time and link
 * them as `const` flash data so the MD core doesn't need to allocate
 * ~158 KB of RAM for tables that never change. The pointers in
 * ym2612_genplus.c (under YM2612_TABLES_IN_FLASH) aim at the const
 * arrays this tool emits.
 *
 * Mirrors the math in vendor/picodrive/pico/sound/ym2612_genplus.c
 * init_tables() exactly. If that file's constants change, this
 * generator must be updated to match.
 *
 * Run from CMake; output is a single .c file with three arrays.
 */
#include <stdio.h>
#include <stdint.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Mirror of ym2612_genplus.c constants. ----------------------------- */

#define ENV_BITS    10
#define ENV_LEN     (1 << ENV_BITS)
#define ENV_STEP    (128.0 / ENV_LEN)        /* 0.125 */

#define SIN_BITS    10
#define SIN_LEN     (1 << SIN_BITS)          /* 1024 */
#define SIN_MASK    (SIN_LEN - 1)

#define TL_RES_LEN  256
#define TL_TAB_LEN  (13 * 2 * TL_RES_LEN)    /* 6656 entries */

#define LFO_PM_LEN  (128 * 8 * 32)           /* 32768 entries */

/* lfo_pm_output[7*8][8] — copied verbatim from
 * vendor/picodrive/pico/sound/ym2612_genplus.c lines 478..550.
 * If you update one, update the other. */
static const uint8_t lfo_pm_output[7*8][8] = {
/* FNUM BIT 4 */
{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,1,1,1,1},
/* FNUM BIT 5 */
{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,1,1,1,1},{0,0,1,1,2,2,2,3},
/* FNUM BIT 6 */
{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
{0,0,0,0,0,0,0,1},{0,0,0,0,1,1,1,1},{0,0,1,1,2,2,2,3},{0,0,2,3,4,4,5,6},
/* FNUM BIT 7 */
{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,1,1},{0,0,0,0,1,1,1,1},
{0,0,0,1,1,1,1,2},{0,0,1,1,2,2,2,3},{0,0,2,3,4,4,5,6},{0,0,4,6,8,8,0xa,0xc},
/* FNUM BIT 8 */
{0,0,0,0,0,0,0,0},{0,0,0,0,1,1,1,1},{0,0,0,1,1,1,2,2},{0,0,1,1,2,2,3,3},
{0,0,1,2,2,2,3,4},{0,0,2,3,4,4,5,6},{0,0,4,6,8,8,0xa,0xc},{0,0,8,0xc,0x10,0x10,0x14,0x18},
/* FNUM BIT 9 */
{0,0,0,0,0,0,0,0},{0,0,0,0,2,2,2,2},{0,0,0,2,2,2,4,4},{0,0,2,2,4,4,6,6},
{0,0,2,4,4,4,6,8},{0,0,4,6,8,8,0xa,0xc},{0,0,8,0xc,0x10,0x10,0x14,0x18},{0,0,0x10,0x18,0x20,0x20,0x28,0x30},
/* FNUM BIT 10 */
{0,0,0,0,0,0,0,0},{0,0,0,0,4,4,4,4},{0,0,0,4,4,4,8,8},{0,0,4,4,8,8,0xc,0xc},
{0,0,4,8,8,8,0xc,0x10},{0,0,8,0xc,0x10,0x10,0x14,0x18},{0,0,0x10,0x18,0x20,0x20,0x28,0x30},{0,0,0x20,0x30,0x40,0x40,0x50,0x60},
};

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s out.c\n", argv[0]);
        return 1;
    }
    FILE *f = fopen(argv[1], "w");
    if (!f) { perror(argv[1]); return 1; }

    static int32_t  tl_tab[TL_TAB_LEN];
    static uint32_t sin_tab[SIN_LEN];
    static int32_t  lfo_pm[LFO_PM_LEN];

    /* ---- tl_tab (Linear Power Table) ----------------------------- */
    for (int x = 0; x < TL_RES_LEN; x++) {
        double m = (1 << 16) / pow(2.0, (x + 1) * (ENV_STEP / 4.0) / 8.0);
        m = floor(m);

        int n = (int)m;     /* 16 bits */
        n >>= 4;            /* 12 bits */
        if (n & 1) n = (n >> 1) + 1;   /* round to nearest */
        else       n = n >> 1;
        n <<= 2;            /* 13 bits (as in real chip) */

        tl_tab[ x*2 + 0 ] =  n;
        tl_tab[ x*2 + 1 ] = -n;

        for (int i = 1; i < 13; i++) {
            int v = n >> i;
            tl_tab[ x*2 + 0 + i * 2 * TL_RES_LEN ] =  v;
            tl_tab[ x*2 + 1 + i * 2 * TL_RES_LEN ] = -v;
        }
    }

    /* ---- sin_tab (Logarithmic Sinus table) ----------------------- */
    for (int i = 0; i < SIN_LEN; i++) {
        double m = sin(((i * 2) + 1) * M_PI / SIN_LEN);
        double o;
        if (m > 0.0) o = 8.0 * log(1.0 / m) / log(2.0);
        else         o = 8.0 * log(-1.0 / m) / log(2.0);
        o = o / (ENV_STEP / 4.0);

        int n = (int)(2.0 * o);
        if (n & 1) n = (n >> 1) + 1;
        else       n = n >> 1;

        sin_tab[i] = (uint32_t)(n * 2 + (m >= 0.0 ? 0 : 1));
    }

    /* ---- lfo_pm_table -------------------------------------------- */
    for (int i = 0; i < 8; i++) {                    /* 8 PM depths */
        for (int fnum = 0; fnum < 128; fnum++) {     /* 7 bits of F-NUMBER */
            unsigned offset_depth = i;
            for (int step = 0; step < 8; step++) {
                unsigned char value = 0;
                for (int bit_tmp = 0; bit_tmp < 7; bit_tmp++) {
                    if (fnum & (1 << bit_tmp)) {
                        unsigned offset_fnum_bit = bit_tmp * 8;
                        value += lfo_pm_output[offset_fnum_bit + offset_depth][step];
                    }
                }
                /* 32 steps for LFO PM (sinus) */
                lfo_pm[(fnum*32*8) + (i*32) + step      + 0] =  value;
                lfo_pm[(fnum*32*8) + (i*32) + (step^7)  + 8] =  value;
                lfo_pm[(fnum*32*8) + (i*32) + step      +16] = -value;
                lfo_pm[(fnum*32*8) + (i*32) + (step^7)  +24] = -value;
            }
        }
    }

    /* ---- Emit C source ------------------------------------------- */
    fprintf(f, "/* Auto-generated by tools/gen_md_genplus_tab.c. Do not edit. */\n\n");

    /* tl_tab values measured at +/-8168 max, comfortably in int16 range.
     * Packing halves the table (26 KB -> 13 KB) so the whole thing fits
     * inside RP2350's 16 KB XIP cache instead of thrashing it on every
     * per-sample lookup. ARM's ldrsh sign-extends in one cycle. */
    fprintf(f, "const short md_genplus_tl_tab_data[%d] = {\n", TL_TAB_LEN);
    for (int i = 0; i < TL_TAB_LEN; i++) {
        if (tl_tab[i] < -32768 || tl_tab[i] > 32767) {
            fprintf(stderr, "tl_tab[%d]=%d outside int16 range!\n",
                    i, tl_tab[i]);
            return 2;
        }
        fprintf(f, "%d,", tl_tab[i]);
        if ((i & 15) == 15) fputc('\n', f);
    }
    fprintf(f, "};\n\n");

    fprintf(f, "const unsigned int md_genplus_sin_tab_data[%d] = {\n", SIN_LEN);
    for (int i = 0; i < SIN_LEN; i++) {
        fprintf(f, "%uu,", sin_tab[i]);
        if ((i & 15) == 15) fputc('\n', f);
    }
    fprintf(f, "};\n\n");

    fprintf(f, "const signed int md_genplus_lfo_pm_data[%d] = {\n", LFO_PM_LEN);
    for (int i = 0; i < LFO_PM_LEN; i++) {
        fprintf(f, "%d,", lfo_pm[i]);
        if ((i & 15) == 15) fputc('\n', f);
    }
    fprintf(f, "};\n");

    fclose(f);
    return 0;
}
