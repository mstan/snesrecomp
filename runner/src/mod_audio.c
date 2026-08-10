#include "mod_audio.h"

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* The runner's existing APU lock serializes audio-callback mixing against
 * trusted game code issuing play/register/unregister requests. */
extern void RtlApuLock(void);
extern void RtlApuUnlock(void);

#define SNES_MOD_AUDIO_MAX_CLIPS 64
#define SNES_MOD_AUDIO_MAX_VOICES 8
/* Keep hostile/accidental trusted-plugin registration bounded too. */
#define SNES_MOD_AUDIO_MAX_TOTAL_BYTES (16u * 1024u * 1024u)

typedef struct ClipSlot {
    int16_t *samples;
    uint32_t frame_count;
    uint32_t sample_rate;
    uint32_t channels;
} ClipSlot;

typedef struct Voice {
    SNESModAudioClip clip;
    uint64_t phase; /* source-frame position in unsigned 32.32 fixed point */
    int gain_percent;
} Voice;

static ClipSlot s_clips[SNES_MOD_AUDIO_MAX_CLIPS];
static Voice s_voices[SNES_MOD_AUDIO_MAX_VOICES];
static unsigned s_replace_cursor;
static size_t s_clip_bytes;

static ClipSlot *clip_slot(SNESModAudioClip clip) {
    if (clip <= 0 || clip > SNES_MOD_AUDIO_MAX_CLIPS) return NULL;
    if (!s_clips[clip - 1].samples) return NULL;
    return &s_clips[clip - 1];
}

static void stop_all_locked(void) {
    memset(s_voices, 0, sizeof(s_voices));
    s_replace_cursor = 0;
}

SNESModAudioClip snes_mod_audio_register_pcm_s16(
    const int16_t *samples, uint32_t frame_count, uint32_t sample_rate,
    uint32_t channels) {
    if (!samples || frame_count == 0 || sample_rate < 8000 ||
        sample_rate > 192000 ||
        (channels != SNES_MOD_AUDIO_CHANNELS_MONO &&
         channels != SNES_MOD_AUDIO_CHANNELS_STEREO) ||
        frame_count > SIZE_MAX / ((size_t)channels * sizeof(*samples)))
        return SNES_MOD_AUDIO_CLIP_INVALID;

    const size_t bytes = (size_t)frame_count * channels * sizeof(*samples);
    if (bytes > SNES_MOD_AUDIO_MAX_TOTAL_BYTES)
        return SNES_MOD_AUDIO_CLIP_INVALID;

    RtlApuLock();
    if (s_clip_bytes > SNES_MOD_AUDIO_MAX_TOTAL_BYTES - bytes) {
        RtlApuUnlock();
        return SNES_MOD_AUDIO_CLIP_INVALID;
    }
    int index;
    for (index = 0; index < SNES_MOD_AUDIO_MAX_CLIPS; ++index)
        if (!s_clips[index].samples) break;
    if (index == SNES_MOD_AUDIO_MAX_CLIPS) {
        RtlApuUnlock();
        return SNES_MOD_AUDIO_CLIP_INVALID;
    }
    int16_t *copy = (int16_t *)malloc(bytes);
    if (!copy) {
        RtlApuUnlock();
        return SNES_MOD_AUDIO_CLIP_INVALID;
    }
    memcpy(copy, samples, bytes);
    s_clips[index].samples = copy;
    s_clips[index].frame_count = frame_count;
    s_clips[index].sample_rate = sample_rate;
    s_clips[index].channels = channels;
    s_clip_bytes += bytes;
    RtlApuUnlock();
    return index + 1;
}

void snes_mod_audio_unregister(SNESModAudioClip clip) {
    RtlApuLock();
    ClipSlot *slot = clip_slot(clip);
    if (slot) {
        for (int i = 0; i < SNES_MOD_AUDIO_MAX_VOICES; ++i)
            if (s_voices[i].clip == clip) memset(&s_voices[i], 0, sizeof(Voice));
        s_clip_bytes -= (size_t)slot->frame_count * slot->channels *
                        sizeof(*slot->samples);
        free(slot->samples);
        memset(slot, 0, sizeof(*slot));
    }
    RtlApuUnlock();
}

int snes_mod_audio_play(SNESModAudioClip clip, int gain_percent) {
    RtlApuLock();
    if (!clip_slot(clip)) {
        RtlApuUnlock();
        return 0;
    }
    if (gain_percent < 0) gain_percent = 0;
    if (gain_percent > 200) gain_percent = 200;
    int index;
    for (index = 0; index < SNES_MOD_AUDIO_MAX_VOICES; ++index)
        if (s_voices[index].clip == SNES_MOD_AUDIO_CLIP_INVALID) break;
    if (index == SNES_MOD_AUDIO_MAX_VOICES) {
        index = (int)s_replace_cursor;
        s_replace_cursor = (s_replace_cursor + 1u) % SNES_MOD_AUDIO_MAX_VOICES;
    }
    s_voices[index].clip = clip;
    s_voices[index].phase = 0;
    s_voices[index].gain_percent = gain_percent;
    RtlApuUnlock();
    return 1;
}

void snes_mod_audio_stop_all(void) {
    RtlApuLock();
    stop_all_locked();
    RtlApuUnlock();
}

static int32_t interpolated_sample(const ClipSlot *slot, uint32_t frame,
                                   uint32_t fraction, uint32_t channel) {
    const uint32_t source_channel = slot->channels == 1 ? 0 : channel;
    const size_t first = (size_t)frame * slot->channels + source_channel;
    const int32_t s0 = slot->samples[first];
    const int32_t s1 = frame + 1 < slot->frame_count
        ? slot->samples[first + slot->channels] : s0;
    return s0 + (int32_t)(((int64_t)(s1 - s0) * fraction) / 4294967296LL);
}

static int32_t sample_for_output(const ClipSlot *slot, uint32_t frame,
                                 uint32_t fraction, uint32_t output_channel,
                                 uint32_t output_channels) {
    if (output_channels == 1 && slot->channels == 2) {
        const int32_t left = interpolated_sample(slot, frame, fraction, 0);
        const int32_t right = interpolated_sample(slot, frame, fraction, 1);
        return (left + right) / 2;
    }
    return interpolated_sample(slot, frame, fraction, output_channel);
}

static int16_t clamp_s16(int64_t value) {
    if (value > INT16_MAX) return INT16_MAX;
    if (value < INT16_MIN) return INT16_MIN;
    return (int16_t)value;
}

void snes_mod_audio_mix(int16_t *dst, int frame_count, uint32_t output_rate,
                        uint32_t output_channels) {
    if (!dst || frame_count <= 0 || output_rate < 8000 || output_rate > 192000 ||
        (output_channels != 1 && output_channels != 2))
        return;
    for (int frame = 0; frame < frame_count; ++frame) {
        int64_t mixed[2] = { dst[(size_t)frame * output_channels], 0 };
        if (output_channels == 2) mixed[1] = dst[(size_t)frame * 2 + 1];
        for (int voice_index = 0; voice_index < SNES_MOD_AUDIO_MAX_VOICES;
             ++voice_index) {
            Voice *voice = &s_voices[voice_index];
            if (voice->clip == SNES_MOD_AUDIO_CLIP_INVALID) continue;
            ClipSlot *slot = clip_slot(voice->clip);
            const uint32_t source_frame = (uint32_t)(voice->phase >> 32);
            if (!slot || source_frame >= slot->frame_count) {
                memset(voice, 0, sizeof(*voice));
                continue;
            }
            const uint32_t fraction = (uint32_t)voice->phase;
            for (uint32_t channel = 0; channel < output_channels; ++channel) {
                const int32_t sample = sample_for_output(
                    slot, source_frame, fraction, channel, output_channels);
                mixed[channel] += ((int64_t)sample * voice->gain_percent) / 100;
            }
            const uint64_t step =
                ((uint64_t)slot->sample_rate << 32) / output_rate;
            if (voice->phase > UINT64_MAX - step)
                memset(voice, 0, sizeof(*voice));
            else
                voice->phase += step;
            if (voice->clip != SNES_MOD_AUDIO_CLIP_INVALID &&
                (voice->phase >> 32) >= slot->frame_count)
                memset(voice, 0, sizeof(*voice));
        }
        for (uint32_t channel = 0; channel < output_channels; ++channel)
            dst[(size_t)frame * output_channels + channel] = clamp_s16(mixed[channel]);
    }
}
