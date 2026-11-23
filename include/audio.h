#ifndef SOS2_AUDIO_H
#define SOS2_AUDIO_H
#include "fdt.h"
#include "vm.h"

/* Audio output, SYS_AUDIO (include/user_abi.h): each process may open one
 * stream, a queue of about half a second of frames, and the mixer sums the
 * playing streams for the output device. Without a device the streams still
 * play, silently, at USER_AUDIO_RATE. All of it runs on CPU 0. */
void audio_init(const struct platform_devices *);
/* From the main loop: feeds the device, or plays the streams silently, as
 * far as real time has gone. */
void audio_poll(void);
int32_t audio_call(unsigned pid, const struct vm_space *, unsigned op, user_ptr argument,
                   uint32_t count);
/* A process stopped: its stream closes, and the frames it still queued are
 * dropped. */
void audio_close_process(unsigned pid);
/* Whether a process's stream holds frames to play and is not paused. */
bool audio_process_playing(unsigned pid);

/* A frame as SYS_AUDIO carries it: signed 16-bit samples, left then right. */
struct audio_frame {
    int16_t left, right;
};

/* An output device, which takes the mixer's frames as it has room for them. */
struct audio_device {
    /* Takes back what the device played and queues more; false once the
     * device has failed, which leaves the streams to play silently. */
    bool (*poll)(void);
    /* Frames the device holds, still to be heard. */
    uint32_t (*latency)(void);
};

/* QEMU's virtio-sound (kernel/audio/virtio_snd.c), the tablet's speakers
 * (boards/gta7lite/audio.c) and the Redmi 2's loudspeaker
 * (boards/redmi2/audio.c); the Nexus 5 has no audio driver yet. */
bool virtio_snd_init(const struct platform_devices *, struct audio_device *);
bool gta7lite_audio_init(struct audio_device *);
bool redmi2_audio_init(struct audio_device *);
/* For the device's driver: the most frames a playing stream holds, which the
 * mixer could take now; and the mix of the playing streams' next count
 * frames, silence where one runs short, which counts them played. */
uint32_t audio_ready(void);
void audio_mix(struct audio_frame *out, uint32_t count);

/* The volume a device's driver applies to the mix, which the volume keys
 * step: from 0, silent, to AUDIO_VOLUME_STEPS, full scale, 3 dB a step. */
enum { AUDIO_VOLUME_STEPS = 15 };

/* audio_mix at a volume step, in mono (each channel the mean of the two)
 * for a single loudspeaker. */
void audio_mix_volume(struct audio_frame *out, uint32_t count, unsigned level, bool mono);
#endif
