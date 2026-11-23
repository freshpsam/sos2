#ifndef SOS2_REDMI2_AUDIO_H
#define SOS2_REDMI2_AUDIO_H
#include <stdbool.h>

/* The loudspeaker's volume (audio.c), from 0, silent, to AUDIO_VOLUME_STEPS
 * (include/audio.h), after stepping it by step. */
unsigned redmi2_audio_volume(int step);
/* Whether the sound hardware is off, its clocks with it, as the whole-chip
 * sleep needs it. */
bool redmi2_audio_quiet(void);
/* A volume key (keys.c): +1 for Volume Up, -1 for Volume Down, once per
 * press and repeated while held (include/volume_keys.h); 0 otherwise. */
int redmi2_volume_key(void);
#endif
