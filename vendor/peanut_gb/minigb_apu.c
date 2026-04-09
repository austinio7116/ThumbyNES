/* Wrapper that ensures config defines are set before including the implementation */
#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE 22050
#endif
#ifndef MINIGB_APU_AUDIO_FORMAT_S16SYS
#define MINIGB_APU_AUDIO_FORMAT_S16SYS
#endif
#include "minigb_apu_impl.c"
