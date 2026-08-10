/* Small host-side PCM overlay mixer for trusted, statically linked mods.
 *
 * Registration copies immutable signed-16 PCM, so loaders can release their
 * input immediately. A host calls snes_mod_audio_mix() from its existing audio
 * pipeline; this module never creates an audio device or clock of its own.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int SNESModAudioClip;

#define SNES_MOD_AUDIO_CLIP_INVALID 0
#define SNES_MOD_AUDIO_CHANNELS_MONO 1u
#define SNES_MOD_AUDIO_CHANNELS_STEREO 2u

/* Copies interleaved mono/stereo s16 PCM. sample_rate must be 8000..192000.
 * The module caps all copied clip data at 16 MiB. Returns a positive handle
 * or SNES_MOD_AUDIO_CLIP_INVALID. */
SNESModAudioClip snes_mod_audio_register_pcm_s16(
    const int16_t *samples, uint32_t frame_count, uint32_t sample_rate,
    uint32_t channels);

/* Stops voices using this clip and releases the copied source data. */
void snes_mod_audio_unregister(SNESModAudioClip clip);

/* Starts an overlapping one-shot. gain_percent is clamped to 0..200. */
int snes_mod_audio_play(SNESModAudioClip clip, int gain_percent);

/* Stop every voice while retaining registrations. Call on reset and after a
 * successful save-state load because host delivery state is not serialized. */
void snes_mod_audio_stop_all(void);

/* Saturating-mix active one-shots into interleaved mono/stereo destination
 * PCM. output_rate is the actual device rate for this callback. The host calls
 * this from its existing audio lock; control functions take that same lock. */
void snes_mod_audio_mix(int16_t *dst, int frame_count, uint32_t output_rate,
                        uint32_t output_channels);

#ifdef __cplusplus
}
#endif
