/* s3eSound (sample channels) and s3eAudio (music), as the game uses them.
 *
 * The loader's s3eSound stubs are in alphabetical order (0x2bde64-0x2bdf90):
 *
 *   0x34d94 ChannelGetInt   0x35d70 ChannelPause    0x35ba8 ChannelPlay
 *   0x35a14 ChannelRegister 0x35cc0 ChannelResume   0x34c14 ChannelSetInt
 *   0x34778 ChannelStop     0x35834 ChannelUnRegister
 *   0x348ec GetFreeChannel  0x34a20 GetInt          0x34898 PauseAllChannels
 *   0x34864 ResumeAllChannels 0x34928 SetInt        0x34828 StopAllChannels
 *
 * and s3eAudio's, with two of the generic callback registry's in between:
 *
 *   0x11a38 GetInt  0x11964 IsCodecSupported  0x11c04 IsPlaying  0x11efc Pause
 *   0x11da8 Play    0x171b0 Register          0x11bc8 Resume     0x11c40 SetInt
 *   0x11b90 Stop    0x17c88 UnRegister
 *
 * How the game plays sound (IwSound): it registers two callbacks on every
 * channel, S3E_CHANNEL_GEN_AUDIO (1) at 0x1638c8 and S3E_CHANNEL_END_SAMPLE
 * (0) at 0xd5934, and starts samples with s3eSoundChannelPlay. The
 * GEN_AUDIO callback makes the channel's audio itself (resampling by the
 * channel's rate, applying its volume) into a buffer it is handed:
 *
 *   s3eSoundGenAudioInfo: 0x00 channel, 0x04 int16 *target, 0x08 numSamples,
 *     0x0c mix (add to target), 0x10 origStart, 0x14 origNumSamples,
 *     0x18 origRepeat, 0x1c endSample (byte, set by the callback at the end)
 *
 * returning the samples written. END_SAMPLE gets {channel, repsRemaining}.
 * Channel properties (ChannelGetInt/SetInt): 0 pitch, 1 rate, 2 user
 * variable, 3 volume, 4 status (read), 5 paused (read).
 *
 * The mixer runs on the game thread, from s3eDeviceYield (sound_pump): it
 * keeps LATENCY samples mixed ahead of what the output has played, calling
 * each playing channel's GEN_AUDIO callback in mix mode into one 22050 Hz
 * mono buffer, so guest code never runs on two threads at once. Channels
 * without a GEN_AUDIO callback play their 16-bit samples directly. The output
 * is an AudioQueue (macOS and iOS) fed from a ring buffer.
 *
 *   RT_SOUND=0          no output device (the host default is no device;
 *                       RT_SOUND=1 turns it on there, iOS always has one)
 *   RT_SOUND_DUMP=f.wav also write the mix to a WAV file
 *   RT_LOG_SOUND=1      log channel calls
 *
 * Without a device, mixing still follows the clock so that samples end and
 * channels free up as they would.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>

#include "rt.h"

#define NCHANNELS 16
#define OUTPUT_RATE 22050

typedef struct {
    int playing, paused;
    uint32_t start, samples, repeat, loopfrom;
    int32_t volume, rate, pitch, uservar;
    uint32_t cb_fn[4], cb_ud[4];
    uint64_t pos;   /* direct playback: 32.32 position in samples */
} Channel;
static Channel ch[NCHANNELS] = {[0 ... NCHANNELS - 1] = {.volume = 256, .rate = OUTPUT_RATE}};
static int32_t master_volume = 256;
static int log_sound = -1;

#define SLOG(...) do { if (log_sound < 0) log_sound = getenv("RT_LOG_SOUND") != NULL; \
                       if (log_sound) rt_log(__VA_ARGS__); } while (0)

static uint64_t produced;   /* samples mixed so far */

static Channel *chan(uint32_t n) { return n < NCHANNELS ? &ch[n] : NULL; }

/* ---------------------------------------------------------------- s3eSound */
static void s3eSoundGetInt(CPU *c) {
    int32_t v;
    switch (arg(c, 0)) {
    case 0: v = master_volume; break;   /* VOLUME */
    case 1: v = OUTPUT_RATE; break;     /* DEFAULT_FREQ */
    case 2: v = OUTPUT_RATE; break;     /* OUTPUT_FREQ */
    case 3: v = NCHANNELS; break;       /* NUM_CHANNELS */
    case 4: {                           /* USED_CHANNELS: a mask */
        v = 0;
        for (int k = 0; k < NCHANNELS; k++) if (ch[k].playing) v |= 1 << k;
        break;
    }
    case 5: v = 1; break;               /* AVAILABLE */
    case 6: v = 256; break;             /* VOLUME_DEFAULT */
    default: v = 0; break;
    }
    ret32(c, (uint32_t)v);
}
static void s3eSoundSetInt(CPU *c) {
    SLOG("s3eSoundSetInt(%u, %d)", arg(c, 0), (int32_t)arg(c, 1));
    if (arg(c, 0) == 0) master_volume = (int32_t)arg(c, 1);
    ret32(c, 0);
}
static void s3eSoundGetFreeChannel(CPU *c) {
    for (int k = 0; k < NCHANNELS; k++)
        if (!ch[k].playing) { ret32(c, (uint32_t)k); return; }
    ret32(c, (uint32_t)-1);
}
static void s3eSoundChannelPlay(CPU *c) {   /* (channel, int16 *start, numSamples, repeat, loopfrom) */
    Channel *h = chan(arg(c, 0));
    SLOG("s3eSoundChannelPlay(%u, %08x, %u samples, repeat %d, loop from %u) at %.2f s", arg(c, 0), arg(c, 1),
         arg(c, 2), (int32_t)arg(c, 3), arg(c, 4), (double)produced / OUTPUT_RATE);
    if (!h) { ret32(c, 1); return; }
    h->start = arg(c, 1); h->samples = arg(c, 2); h->repeat = arg(c, 3); h->loopfrom = arg(c, 4);
    h->playing = 1; h->paused = 0; h->pos = 0;
    ret32(c, 0);
}
static void s3eSoundChannelStop(CPU *c) {
    Channel *h = chan(arg(c, 0));
    if (h) h->playing = h->paused = 0;
    ret32(c, h ? 0 : 1);
}
static void s3eSoundChannelPause(CPU *c) { Channel *h = chan(arg(c, 0)); if (h) h->paused = 1; ret32(c, h ? 0 : 1); }
static void s3eSoundChannelResume(CPU *c) { Channel *h = chan(arg(c, 0)); if (h) h->paused = 0; ret32(c, h ? 0 : 1); }
static void s3eSoundChannelGetInt(CPU *c) {
    Channel *h = chan(arg(c, 0));
    int32_t v = 0;
    if (h) switch (arg(c, 1)) {
        case 0: v = (int32_t)((int64_t)h->rate * 256 / OUTPUT_RATE); break;   /* PITCH, 8.8 */
        case 1: v = h->rate; break;                                          /* RATE */
        case 2: v = h->uservar; break;                                       /* USERVAR */
        case 3: v = h->volume; break;                                        /* VOLUME */
        case 4: v = h->playing; break;                                       /* STATUS */
        case 5: v = h->paused; break;                                        /* PAUSED */
        }
    ret32(c, (uint32_t)v);
}
static void s3eSoundChannelSetInt(CPU *c) {
    Channel *h = chan(arg(c, 0));
    int32_t v = (int32_t)arg(c, 2);
    if (h) switch (arg(c, 1)) {
        case 0: h->rate = (int32_t)((int64_t)(v < 0 ? 0 : v) * OUTPUT_RATE / 256); break;   /* PITCH */
        case 1: h->rate = v < 0 ? 0 : v > 0x40000 ? 0x40000 : v; break;                  /* RATE */
        case 2: h->uservar = v; break;
        case 3: h->volume = v < 0 ? 0 : v > 256 ? 256 : v; break;                          /* VOLUME */
        }
    ret32(c, h ? 0 : 1);
}
static void s3eSoundChannelRegister(CPU *c) {   /* (channel, cbid, fn, userdata) */
    Channel *h = chan(arg(c, 0));
    SLOG("s3eSoundChannelRegister(%u, cb %u, fn %08x, ud %08x)", arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3));
    if (!h || arg(c, 1) > 3) { ret32(c, 1); return; }
    h->cb_fn[arg(c, 1)] = arg(c, 2);
    h->cb_ud[arg(c, 1)] = arg(c, 3);
    ret32(c, 0);
}
static void s3eSoundChannelUnRegister(CPU *c) {
    Channel *h = chan(arg(c, 0));
    if (h && arg(c, 1) <= 3) h->cb_fn[arg(c, 1)] = 0;
    ret32(c, h ? 0 : 1);
}
static void s3eSoundPauseAll(CPU *c) { for (int k = 0; k < NCHANNELS; k++) ch[k].paused = 1; }
static void s3eSoundResumeAll(CPU *c) { for (int k = 0; k < NCHANNELS; k++) ch[k].paused = 0; }
static void s3eSoundStopAll(CPU *c) { for (int k = 0; k < NCHANNELS; k++) ch[k].playing = ch[k].paused = 0; }

/* ----------------------------------------------------------------- mixer */
#define LATENCY 2048    /* samples mixed ahead of the output (~93 ms) */
#define BLOCK 256
#define RING 16384      /* power of two */

static int16_t ring[RING];
static _Atomic uint64_t ring_read, ring_write;   /* in samples */
static int have_device = -1;
static struct timespec t_start;
static FILE *dump;
static uint32_t dump_samples;
static uint32_t gen_info, gen_target, end_info;  /* guest buffers */

static void wav_header(FILE *f, uint32_t n) {
    uint32_t bytes = n * 2;
    uint8_t h[44] = {'R','I','F','F'};
    uint32_t v;
    v = 36 + bytes; memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4);
    uint16_t w = 1; memcpy(h + 20, &w, 2);           /* PCM */
    w = 1; memcpy(h + 22, &w, 2);                    /* mono */
    v = OUTPUT_RATE; memcpy(h + 24, &v, 4);
    v = OUTPUT_RATE * 2; memcpy(h + 28, &v, 4);
    w = 2; memcpy(h + 32, &w, 2);
    w = 16; memcpy(h + 34, &w, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &bytes, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
    fseek(f, 0, SEEK_END);
}

/* ------------------------------------------------------- output device */
static AudioQueueRef queue;

static void aq_callback(void *ud, AudioQueueRef q, AudioQueueBufferRef b) {
    (void)ud;
    int16_t *out = b->mAudioData;
    uint32_t n = b->mAudioDataBytesCapacity / 2;
    uint64_t r = atomic_load(&ring_read), w = atomic_load(&ring_write);
    uint32_t k = 0;
    for (; k < n && r < w; k++, r++) out[k] = ring[r & (RING - 1)];
    for (; k < n; k++) out[k] = 0;   /* underrun: silence */
    atomic_store(&ring_read, r);
    b->mAudioDataByteSize = n * 2;
    AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

static void device_start(void) {
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = OUTPUT_RATE;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    f.mChannelsPerFrame = 1;
    f.mBitsPerChannel = 16;
    f.mBytesPerFrame = 2;
    f.mFramesPerPacket = 1;
    f.mBytesPerPacket = 2;
    OSStatus e = AudioQueueNewOutput(&f, aq_callback, NULL, NULL, NULL, 0, &queue);
    if (e) { rt_log("sound: no audio queue (%d)", (int)e); have_device = 0; return; }
    for (int k = 0; k < 3; k++) {
        AudioQueueBufferRef b;
        if (AudioQueueAllocateBuffer(queue, 512 * 2, &b) == 0) {
            memset(b->mAudioData, 0, 1024);
            b->mAudioDataByteSize = 1024;
            AudioQueueEnqueueBuffer(queue, b, 0, NULL);
        }
    }
    e = AudioQueueStart(queue, NULL);
    if (e) { rt_log("sound: audio queue didn't start (%d)", (int)e); have_device = 0; return; }
    rt_log("sound: output %d Hz mono", OUTPUT_RATE);
}

static void sound_init(void) {
    clock_gettime(CLOCK_MONOTONIC, &t_start);
    const char *e = getenv("RT_SOUND");
#if TARGET_OS_IPHONE
    have_device = !(e && !strcmp(e, "0"));
#else
    have_device = e && strcmp(e, "0");
#endif
    if (have_device) device_start();
    const char *d = getenv("RT_SOUND_DUMP");
    if (d && (dump = fopen(d, "wb"))) wav_header(dump, 0);
}

/* Samples the output has taken (device) or would have (the clock). */
static uint64_t consumed(void) {
    if (have_device) return atomic_load(&ring_read);
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    double s = (double)(t.tv_sec - t_start.tv_sec) + (double)(t.tv_nsec - t_start.tv_nsec) / 1e9;
    return (uint64_t)(s * OUTPUT_RATE);
}

static void end_channel(CPU *c, int k) {
    Channel *h = &ch[k];
    h->playing = 0;
    SLOG("channel %d ended at %.2f s", k, (double)produced / OUTPUT_RATE);
    if (h->cb_fn[0]) {
        st32(end_info, (uint32_t)k);
        st32(end_info + 4, 0);
        guest_call(c, h->cb_fn[0], 2, end_info, h->cb_ud[0]);
    }
}

/* One block: every playing channel into mix (int16, BLOCK samples). */
static void mix_block(CPU *c, int16_t *mix) {
    memset(G(gen_target), 0, BLOCK * 2);
    for (int k = 0; k < NCHANNELS; k++) {
        Channel *h = &ch[k];
        if (!h->playing || h->paused) continue;
        if (h->cb_fn[1]) {
            memset(G(gen_info), 0, 0x24);
            st32(gen_info + 0x00, (uint32_t)k);
            st32(gen_info + 0x04, gen_target);
            st32(gen_info + 0x08, BLOCK);
            st32(gen_info + 0x0c, 1);   /* mix into the target */
            st32(gen_info + 0x10, h->start);
            st32(gen_info + 0x14, h->samples);
            st32(gen_info + 0x18, h->repeat);
            guest_call(c, h->cb_fn[1], 2, gen_info, h->cb_ud[1]);
            if (ld8(gen_info + 0x1c)) end_channel(c, k);
        } else {
            /* 16-bit mono at the channel's rate */
            uint64_t step = ((uint64_t)(uint32_t)h->rate << 32) / OUTPUT_RATE;
            int16_t *t = (int16_t *)G(gen_target);
            for (int n = 0; n < BLOCK; n++) {
                uint32_t i = (uint32_t)(h->pos >> 32);
                if (i >= h->samples) {
                    if (h->repeat == 1 || h->samples == 0) { end_channel(c, k); break; }
                    if (h->repeat > 1) h->repeat--;
                    h->pos = (uint64_t)h->loopfrom << 32;
                    i = h->loopfrom;
                }
                int32_t v = (int16_t)ld16(h->start + 2 * i) * h->volume / 256 + t[n];
                t[n] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                h->pos += step;
            }
        }
    }
    const int16_t *t = (const int16_t *)G(gen_target);
    for (int n = 0; n < BLOCK; n++) mix[n] = (int16_t)(t[n] * master_volume / 256);
}

static void sound_pump_inner(CPU *c);

/* From s3eDeviceYield: keep LATENCY samples mixed ahead of the output. */
void sound_pump(CPU *c) {
    static uint64_t calls, blocks, ns, last;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t before = produced;
    sound_pump_inner(c);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    calls++;
    blocks += (produced - before) / BLOCK;
    ns += (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000000LL + (t1.tv_nsec - t0.tv_nsec));
    if (getenv("RT_SOUND_PROFILE") && t1.tv_sec - (time_t)last >= 5) {
        last = (uint64_t)t1.tv_sec;
        rt_log("sound: %llu pumps, %llu blocks, %.1f ms in the last 5 s; produced %.1f s, consumed %.1f s",
               (unsigned long long)calls, (unsigned long long)blocks, ns / 1e6, (double)produced / OUTPUT_RATE,
               (double)consumed() / OUTPUT_RATE);
        calls = blocks = ns = 0;
    }
}

static void sound_pump_inner(CPU *c) {
    if (have_device < 0) sound_init();
    if (!gen_info) {
        gen_info = guest_alloc(0x40);
        end_info = guest_alloc(16);
        gen_target = guest_alloc(BLOCK * 2);
    }
    int any = 0;
    for (int k = 0; k < NCHANNELS; k++) any |= ch[k].playing;
    uint64_t want = consumed() + LATENCY;
    if (produced + 8 * LATENCY < want) produced = want - LATENCY;   /* far behind (a long load): skip */
    while (produced < want) {
        int16_t mix[BLOCK];
        if (any) mix_block(c, mix);
        else memset(mix, 0, sizeof mix);
        if (have_device) {
            uint64_t w = atomic_load(&ring_write);
            for (int n = 0; n < BLOCK; n++) ring[(w + n) & (RING - 1)] = mix[n];
            atomic_store(&ring_write, w + BLOCK);
        }
        if (dump) {
            fwrite(mix, 2, BLOCK, dump);
            dump_samples += BLOCK;
            if ((dump_samples & 0xffff) == 0) { wav_header(dump, dump_samples); fflush(dump); }
        }
        produced += BLOCK;
    }
}

/* ---------------------------------------------------------------- s3eAudio */
static void s3eAudioPlay(CPU *c) {   /* (filename, repeatCount) */
    SLOG("s3eAudioPlay(\"%s\", %u)", gstr(arg(c, 0)) ? gstr(arg(c, 0)) : "", arg(c, 1));
    ret32(c, 1);   /* S3E_RESULT_ERROR: nothing plays yet */
}
static void s3eAudioIsCodecSupported(CPU *c) {
    SLOG("s3eAudioIsCodecSupported(%u)", arg(c, 0));
    ret32(c, 0);
}
static void s3eAudioIsPlaying(CPU *c) { ret32(c, 0); }
static void s3eAudioSetInt(CPU *c) { SLOG("s3eAudioSetInt(%u, %d)", arg(c, 0), (int32_t)arg(c, 1)); ret32(c, 0); }
static void s3eAudioGetInt(CPU *c) { ret32(c, arg(c, 0) == 0 ? 256 : 0); }
static void s3eAudioStopPauseResume(CPU *c) { ret32(c, 0); }

const ImportImpl g_sound_impls[] = {
    {0x00034d94, "s3eSoundChannelGetInt", s3eSoundChannelGetInt},
    {0x00035d70, "s3eSoundChannelPause", s3eSoundChannelPause},
    {0x00035ba8, "s3eSoundChannelPlay", s3eSoundChannelPlay},
    {0x00035a14, "s3eSoundChannelRegister", s3eSoundChannelRegister},
    {0x00035cc0, "s3eSoundChannelResume", s3eSoundChannelResume},
    {0x00034c14, "s3eSoundChannelSetInt", s3eSoundChannelSetInt},
    {0x00034778, "s3eSoundChannelStop", s3eSoundChannelStop},
    {0x00035834, "s3eSoundChannelUnRegister", s3eSoundChannelUnRegister},
    {0x000348ec, "s3eSoundGetFreeChannel", s3eSoundGetFreeChannel},
    {0x00034a20, "s3eSoundGetInt", s3eSoundGetInt},
    {0x00034898, "s3eSoundPauseAllChannels", s3eSoundPauseAll},
    {0x00034864, "s3eSoundResumeAllChannels", s3eSoundResumeAll},
    {0x00034928, "s3eSoundSetInt", s3eSoundSetInt},
    {0x00034828, "s3eSoundStopAllChannels", s3eSoundStopAll},
    {0x00011a38, "s3eAudioGetInt", s3eAudioGetInt},
    {0x00011964, "s3eAudioIsCodecSupported", s3eAudioIsCodecSupported},
    {0x00011c04, "s3eAudioIsPlaying", s3eAudioIsPlaying},
    {0x00011efc, "s3eAudioPause", s3eAudioStopPauseResume},
    {0x00011da8, "s3eAudioPlay", s3eAudioPlay},
    {0x00011bc8, "s3eAudioResume", s3eAudioStopPauseResume},
    {0x00011c40, "s3eAudioSetInt", s3eAudioSetInt},
    {0x00011b90, "s3eAudioStop", s3eAudioStopPauseResume},
};
const uint32_t g_sound_impl_count = sizeof g_sound_impls / sizeof g_sound_impls[0];
