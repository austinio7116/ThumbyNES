/*
 * ThumbyNES YM2612 shim — PicoDrive API → Genesis Plus GX YM2612.
 *
 * Activated by the CMake option THUMBYNES_YM2612_GENPLUS=ON, which
 * excludes pico/sound/ym2612.c from the build and compiles this file
 * plus ym2612_genplus.c instead. The shim is responsible for:
 *
 *   - Defining the PicoDrive-API entry points the rest of the engine
 *     expects (YM2612Init_, YM2612ResetChip_, YM2612Write_,
 *     YM2612UpdateOne_, YM2612Shutdown_, YM2612PicoTick_,
 *     YM2612PicoStateLoad_, YM2612PicoState{Save,Load}{2,3},
 *     YM2612GetRegs).
 *
 *   - Defining the global `YM2612 ym2612` struct that pico/memory.c
 *     reads/writes directly (dacen, dacout, REGS[], OPN.ST.address /
 *     mode / TA / TB / status, addr_A1, etc.). Memory.c keeps this
 *     struct authoritative for timer accounting; the shim mirrors
 *     into the GenPlus internal state at update time.
 *
 *   - Bridging the call to GenPlus's YM2612Update (always stereo,
 *     ~53 kHz) to PicoDrive's expected stereo/mono buffer with
 *     "add to existing" vs "overwrite" semantics for is_buf_empty.
 */

#include <stdlib.h>
#include <string.h>

#include "../pico_types.h"
#include "ym2612.h"
#include "ym2612_genplus.h"

/* Externally-visible PicoDrive-style chip struct. memory.c, sound.c
 * and pico_int.h read and modify these fields directly. We update
 * GenPlus's internal state (OPN.ST.mode, dacen, dacout) from these
 * fields just before each YM2612 update tick. */
YM2612 ym2612;

/* Stereo scratch buffer for the resampler. Allocated lazily on first
 * YM2612UpdateOne_, freed by YM2612Shutdown_. Declared here at file
 * scope so YM2612Shutdown_ (defined before shim_get_tmpbuf) can see
 * the names. */
static int *shim_tmpbuf;
static int  shim_tmpbuf_capacity;

/* ---------------------------------------------------------------- */
/* Init / shutdown / reset                                          */
/* ---------------------------------------------------------------- */

void YM2612Init_(int baseclock, int rate, int flags)
{
    /* baseclock and rate are ignored — GenPlus's YM2612 always runs at
     * the chip-native sample rate (chipclock/144 ≈ 53267 Hz NTSC), and
     * the PicoDrive sound layer applies a polyphase-FIR resampler to
     * the host rate. flags' ST_SSG / ST_DAC bits are advisory in
     * GenPlus; SSG-EG is always implemented and the DAC is always on
     * when register 0x2b enables it. */
    (void)baseclock;
    (void)rate;

    YMGP_Init();
    YMGP_Config(YMGP_DISCRETE);
    YMGP_ResetChip();

    memset(&ym2612, 0, sizeof(ym2612));
    ym2612.OPN.ST.flags = (UINT8)flags;
}

void YM2612ResetChip_(void)
{
    YMGP_ResetChip();
    /* Mirror init's effect on the shadow struct's status / DAC bits
     * without clearing register history (memory.c's
     * ym2612_unpack_state_old replays REGS[] after a reset). */
    ym2612.OPN.ST.status = 0;
    ym2612.OPN.ST.mode   = 0;
    ym2612.dacen         = 0;
    ym2612.dacout        = 0;
}

void YM2612Shutdown_(void)
{
    /* On host: free the heap-allocated tl_tab/sin_tab/lfo_pm_table so
     * sibling emulator cores can reclaim ~158 KB. On device with
     * YM2612_TABLES_IN_FLASH the tables live in const flash and
     * YMGP_Shutdown is a no-op. Also drop the resampler tmpbuf. */
    YMGP_Shutdown();

    free(shim_tmpbuf);
    shim_tmpbuf          = NULL;
    shim_tmpbuf_capacity = 0;
}

/* ---------------------------------------------------------------- */
/* Register write                                                   */
/* ---------------------------------------------------------------- */

int YM2612Write_(unsigned int a, unsigned int v)
{
    /* memory.c is the canonical source for the address latch and the
     * REGS[] shadow. It also short-circuits writes to 0x24/0x25/0x26
     * (timers), 0x27 (mode), 0x2a/0x2b (DAC), so this shim only sees
     * "ordinary" register writes, plus address-port writes which we
     * forward verbatim to keep GenPlus's address latch in sync.
     *
     * For port-0 (a=1) data writes we set GenPlus's address from the
     * shadow first; for port-1 (a=3) we use port 1's address namespace
     * (the high register bank). */
    a &= 3;

    switch (a)
    {
    case 0: /* address port 0 */
    case 2: /* address port 1 */
        YMGP_Write(a, v);
        return 0;

    case 1: /* data port 0 */
        /* Push the latched address into GenPlus's port-0 address. */
        YMGP_Write(0, (unsigned int)ym2612.OPN.ST.address & 0xff);
        YMGP_Write(1, v);
        return 0;

    case 3: /* data port 1 */
        YMGP_Write(2, (unsigned int)ym2612.OPN.ST.address & 0xff);
        YMGP_Write(3, v);
        return 0;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* Sample generation                                                */
/* ---------------------------------------------------------------- */

#ifndef YM2612_SHIM_TMPBUF_SAMPLES
/* GenPlus emits stereo lt/rt int32 pairs, and PicoDrive's resampler
 * normally asks for ~1100 samples at a time when running at 53 kHz
 * native (Pico.snd.len ≈ rate/fps). 4096 stereo pairs is comfortable
 * headroom and only ~32 KB of stack-equivalent BSS, but we put it on
 * the heap on first use to avoid a fixed BSS cost when GenPlus is
 * compiled-in but not active. */
#define YM2612_SHIM_TMPBUF_SAMPLES 4096
#endif

static int *shim_get_tmpbuf(int length_stereo_pairs)
{
    if (length_stereo_pairs > shim_tmpbuf_capacity) {
        int newcap = length_stereo_pairs;
        if (newcap < YM2612_SHIM_TMPBUF_SAMPLES) newcap = YM2612_SHIM_TMPBUF_SAMPLES;
        int *p = (int *)realloc(shim_tmpbuf, newcap * 2 * sizeof(int));
        if (!p) return NULL;
        shim_tmpbuf = p;
        shim_tmpbuf_capacity = newcap;
    }
    return shim_tmpbuf;
}

int YM2612UpdateOne_(s32 *buffer, int length, int stereo, int is_buf_empty)
{
    int i;
    int *tmp;

    if (length <= 0)
        return 0;

    /* Push the bits of state that memory.c owns into GenPlus before
     * generating samples. Timer-A/B status is set by memory.c via
     * ym2612_update_status; we don't propagate it (GenPlus's update
     * doesn't read it). */
    YMGP_SetDac(ym2612.dacen ? 1 : 0, ym2612.dacout);
    YMGP_SetModeBits((unsigned int)ym2612.OPN.ST.mode);

    tmp = shim_get_tmpbuf(length);
    if (!tmp) {
        /* Out-of-memory: return "no FM activity" so the caller treats
         * the buffer as silence rather than crashing. */
        if (is_buf_empty)
            memset(buffer, 0, (length << stereo) * sizeof(*buffer));
        return 0;
    }

    /* GenPlus emits clipped stereo (lt, rt, lt, rt, ...) signed ints
     * in the range roughly [-15 bits .. +15 bits]. */
    YMGP_Update(tmp, length);

    if (stereo)
    {
        if (is_buf_empty) {
            for (i = 0; i < length; i++) {
                buffer[(i<<1) + 0] = tmp[(i<<1) + 0];
                buffer[(i<<1) + 1] = tmp[(i<<1) + 1];
            }
        } else {
            for (i = 0; i < length; i++) {
                buffer[(i<<1) + 0] += tmp[(i<<1) + 0];
                buffer[(i<<1) + 1] += tmp[(i<<1) + 1];
            }
        }
    }
    else
    {
        /* Mono: average the two channels. */
        if (is_buf_empty) {
            for (i = 0; i < length; i++)
                buffer[i] = (tmp[(i<<1) + 0] + tmp[(i<<1) + 1]) >> 1;
        } else {
            for (i = 0; i < length; i++)
                buffer[i] += (tmp[(i<<1) + 0] + tmp[(i<<1) + 1]) >> 1;
        }
    }

    /* PicoDrive's caller treats the return value as "did FM produce
     * non-silence?" — a 0 lets sound.c skip the FIR resampler step on
     * idle frames. We can't cheaply tell from outside GenPlus whether
     * any operator is active, so we always return 1 and let downstream
     * volume handling deal with silence. */
    return 1;
}

/* ---------------------------------------------------------------- */
/* Tick / state save (stubs)                                        */
/* ---------------------------------------------------------------- */

int YM2612PicoTick_(int n)
{
    /* PicoDrive's YM2612PicoTick advances its internal LFO/EG counters
     * for the resampler. GenPlus advances them inside its YM2612Update,
     * so this is a no-op for us. */
    (void)n;
    return 0;
}

void YM2612PicoStateLoad_(void)
{
    /* No-op. memory.c's ym2612_unpack_state_old replays REGS[] through
     * ym2612_write_local, which is sufficient to restore GenPlus's FM
     * state from the saved register snapshot. */
}

int YM2612PicoStateLoad2(int *tat, int *tbt, int *busy)
{
    /* Legacy GP2X save format — not used by mdhost. */
    if (tat)  *tat  = 0;
    if (tbt)  *tbt  = 0;
    if (busy) *busy = 0;
    return -1; /* "no saved timers" — caller falls back gracefully. */
}

void YM2612PicoStateSave2(int tat, int tbt, int busy)
{
    (void)tat; (void)tbt; (void)busy;
}

size_t YM2612PicoStateSave3(void *buf_, size_t size)
{
    /* Save the externally-visible REGS[] shadow plus the address latch
     * fields. memory.c will replay register writes on load. */
    u8 *buf = (u8 *)buf_;
    if (!buf || size < sizeof(ym2612.REGS) + 4) return 0;
    memcpy(buf, ym2612.REGS, sizeof(ym2612.REGS));
    buf[sizeof(ym2612.REGS) + 0] = (u8)ym2612.OPN.ST.address;
    buf[sizeof(ym2612.REGS) + 1] = (u8)ym2612.addr_A1;
    buf[sizeof(ym2612.REGS) + 2] = (u8)ym2612.dacen;
    buf[sizeof(ym2612.REGS) + 3] = 0;
    return sizeof(ym2612.REGS) + 4;
}

void YM2612PicoStateLoad3(const void *buf_, size_t size)
{
    const u8 *buf = (const u8 *)buf_;
    if (!buf || size < sizeof(ym2612.REGS) + 4) return;
    memcpy(ym2612.REGS, buf, sizeof(ym2612.REGS));
    ym2612.OPN.ST.address = buf[sizeof(ym2612.REGS) + 0];
    ym2612.addr_A1        = buf[sizeof(ym2612.REGS) + 1];
    ym2612.dacen          = buf[sizeof(ym2612.REGS) + 2];
}

void *YM2612GetRegs(void)
{
    return (void *)ym2612.REGS;
}
