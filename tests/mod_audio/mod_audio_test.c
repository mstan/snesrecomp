#include "mod_audio.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The isolated mixer test supplies the host serialization hooks. */
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

static void test_noop_and_mono(void) {
    int16_t out[4] = { 11, -12, 13, -14 };
    snes_mod_audio_mix(out, 2, 32040, 2);
    assert(out[0] == 11 && out[1] == -12 && out[2] == 13 && out[3] == -14);

    const int16_t pcm[] = { 1000, -1000 };
    SNESModAudioClip clip = snes_mod_audio_register_pcm_s16(
        pcm, 2, 32040, SNES_MOD_AUDIO_CHANNELS_MONO);
    assert(clip != SNES_MOD_AUDIO_CLIP_INVALID);
    assert(snes_mod_audio_play(clip, 50));
    memset(out, 0, sizeof(out));
    snes_mod_audio_mix(out, 2, 32040, 2);
    assert(out[0] == 500 && out[1] == 500);
    assert(out[2] == -500 && out[3] == -500);
    snes_mod_audio_unregister(clip);
}

static void test_overlap_and_saturation(void) {
    const int16_t pcm[] = { 1000 };
    SNESModAudioClip clip = snes_mod_audio_register_pcm_s16(
        pcm, 1, 32040, SNES_MOD_AUDIO_CHANNELS_MONO);
    assert(clip);
    assert(snes_mod_audio_play(clip, 100));
    assert(snes_mod_audio_play(clip, 100));
    int16_t out[2] = { 100, -100 };
    snes_mod_audio_mix(out, 1, 32040, 2);
    assert(out[0] == 2100 && out[1] == 1900);
    snes_mod_audio_unregister(clip);

    const int16_t loud[] = { 30000 };
    clip = snes_mod_audio_register_pcm_s16(
        loud, 1, 32040, SNES_MOD_AUDIO_CHANNELS_MONO);
    assert(clip && snes_mod_audio_play(clip, 200) && snes_mod_audio_play(clip, 200));
    out[0] = -1; out[1] = 1;
    snes_mod_audio_mix(out, 1, 32040, 2);
    assert(out[0] == 32767 && out[1] == 32767);
    snes_mod_audio_unregister(clip);
}

static void test_rate_conversion_and_stereo(void) {
    const int16_t ramp[] = { 0, 1000, 2000 };
    SNESModAudioClip clip = snes_mod_audio_register_pcm_s16(
        ramp, 3, 8000, SNES_MOD_AUDIO_CHANNELS_MONO);
    assert(clip && snes_mod_audio_play(clip, 100));
    int16_t out[8] = {};
    snes_mod_audio_mix(out, 4, 16000, 2);
    assert(out[0] == 0 && out[2] == 500 && out[4] == 1000 && out[6] == 1500);
    assert(out[1] == 0 && out[3] == 500 && out[5] == 1000 && out[7] == 1500);
    snes_mod_audio_unregister(clip);

    const int16_t stereo[] = { 1200, -900 };
    clip = snes_mod_audio_register_pcm_s16(
        stereo, 1, 32040, SNES_MOD_AUDIO_CHANNELS_STEREO);
    assert(clip && snes_mod_audio_play(clip, 100));
    int16_t stereo_out[2] = {};
    snes_mod_audio_mix(stereo_out, 1, 32040, 2);
    assert(stereo_out[0] == 1200 && stereo_out[1] == -900);
    snes_mod_audio_unregister(clip);
}

static void test_stop_and_unregister(void) {
    const int16_t pcm[] = { 1234, 1234 };
    SNESModAudioClip clip = snes_mod_audio_register_pcm_s16(
        pcm, 2, 32040, SNES_MOD_AUDIO_CHANNELS_MONO);
    assert(clip && snes_mod_audio_play(clip, 100));
    snes_mod_audio_stop_all();
    int16_t out[2] = {};
    snes_mod_audio_mix(out, 1, 32040, 2);
    assert(out[0] == 0 && out[1] == 0);
    assert(snes_mod_audio_play(clip, 100));
    snes_mod_audio_unregister(clip);
    assert(!snes_mod_audio_play(clip, 100));
    snes_mod_audio_mix(out, 1, 32040, 2);
    assert(out[0] == 0 && out[1] == 0);
}

static void test_voices_and_loops(void) {
    const int16_t pcm[] = { 100, 200, 300, 400 };
    SNESModAudioClip clip = snes_mod_audio_register_pcm_s16(
        pcm, 4, 32040, SNES_MOD_AUDIO_CHANNELS_MONO);
    assert(clip);
    /* Loop frames 1..3 (exclusive end): 200,300,200,300,... */
    SNESModAudioVoice loop = snes_mod_audio_play_loop(clip, 100, 1, 3);
    assert(loop != SNES_MOD_AUDIO_VOICE_INVALID);
    assert(snes_mod_audio_voice_active(loop));
    int16_t out[12] = {};
    snes_mod_audio_mix(out, 6, 32040, 2);
    /* Phase starts at 0 -> first frame is 100, then wraps inside 1..3. */
    assert(out[0] == 100 && out[2] == 200 && out[4] == 300 && out[6] == 200 &&
           out[8] == 300 && out[10] == 200);
    assert(snes_mod_audio_voice_active(loop));
    /* Gain change applies live. */
    assert(snes_mod_audio_set_voice_gain(loop, 50));
    memset(out, 0, sizeof(out));
    snes_mod_audio_mix(out, 1, 32040, 2);
    assert(out[0] == 150 || out[0] == 100); /* 300*0.5 or 200*0.5 */
    /* Pitch: double speed skips every other frame. */
    assert(snes_mod_audio_set_voice_pitch(loop, 2048));
    snes_mod_audio_set_voice_gain(loop, 100);
    memset(out, 0, sizeof(out));
    snes_mod_audio_mix(out, 4, 32040, 2);
    for (int i = 0; i < 4; i++) assert(out[i * 2] == 200 || out[i * 2] == 300);
    snes_mod_audio_stop_voice(loop);
    assert(!snes_mod_audio_voice_active(loop));
    memset(out, 0, sizeof(out));
    snes_mod_audio_mix(out, 2, 32040, 2);
    assert(out[0] == 0 && out[2] == 0);
    /* One-shot voice handle expires by itself. */
    SNESModAudioVoice shot = snes_mod_audio_play_voice(clip, 100);
    assert(shot && snes_mod_audio_voice_active(shot));
    snes_mod_audio_mix(out, 8, 32040, 2);
    assert(!snes_mod_audio_voice_active(shot));
    assert(!snes_mod_audio_set_voice_gain(shot, 10));
    /* Stale handles from a reused slot are rejected. */
    SNESModAudioVoice again = snes_mod_audio_play_voice(clip, 100);
    assert(again != shot);
    snes_mod_audio_stop_voice(shot); /* must not stop `again` */
    assert(snes_mod_audio_voice_active(again));
    snes_mod_audio_stop_all();
    snes_mod_audio_unregister(clip);
}

int main(void) {
    test_voices_and_loops();
    test_noop_and_mono();
    test_overlap_and_saturation();
    test_rate_conversion_and_stereo();
    test_stop_and_unregister();
    puts("mod audio tests passed");
    return 0;
}
