/* Wrapper that ensures config defines are set before including the implementation.
 * 44100 Hz internal — see gb_core.c for the rationale; the runner
 * decimates to 22050 with anti-alias FIR before publishing samples. */
#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE 44100
#endif
#ifndef MINIGB_APU_AUDIO_FORMAT_S16SYS
#define MINIGB_APU_AUDIO_FORMAT_S16SYS
#endif
#include "minigb_apu_impl.c"
