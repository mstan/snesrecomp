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

/* Voice handles identify a playing instance; 0 is invalid. */
typedef int SNESModAudioVoice;
#define SNES_MOD_AUDIO_VOICE_INVALID 0

/* Starts a one-shot and returns its voice handle so it can be stopped or
 * re-gained early (e.g. a brake sound cut short). */
SNESModAudioVoice snes_mod_audio_play_voice(SNESModAudioClip clip,
                                            int gain_percent);

/* Starts a looping voice (engine hum, charge whine). loop_start_frame /
 * loop_end_frame bound the repeated region; pass 0 / 0 to loop the whole
 * clip. Loops never self-expire: stop them explicitly. */
SNESModAudioVoice snes_mod_audio_play_loop(SNESModAudioClip clip,
                                           int gain_percent,
                                           uint32_t loop_start_frame,
                                           uint32_t loop_end_frame);

/* Adjust a playing voice's gain (0..200) or pitch (source-rate multiplier in
 * 1/1024 units, 1024 = unity, clamped 256..4096). Return 0 when the voice
 * has already ended or the handle is stale. */
int snes_mod_audio_set_voice_gain(SNESModAudioVoice voice, int gain_percent);
int snes_mod_audio_set_voice_pitch(SNESModAudioVoice voice, int pitch_q10);

/* Stop one voice; a stale handle is ignored. */
void snes_mod_audio_stop_voice(SNESModAudioVoice voice);

/* Nonzero while the voice is still playing. */
int snes_mod_audio_voice_active(SNESModAudioVoice voice);

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
