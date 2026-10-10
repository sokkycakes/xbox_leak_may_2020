/*
 * dsound_test: the DirectSound HLE (src/hle/dsound.c) without a title.
 *
 * The test includes dsound.c directly so it can call the static entry points
 * the jmp patches would reach, and stubs what they need from the rest of
 * xbcompat (logging, the pool, contiguous memory, NtSetEvent, the file
 * layer).  The mixer is driven by hand (g_ds_no_audio_thread) so every
 * sample is predictable; the last test opens the real SDL audio device
 * (run with SDL_AUDIODRIVER=dummy) and checks that the clock runs.
 *
 * Covered: buffer playback and mixing of two voices, one-shot end and
 * status, looping and loop regions, pitch / SetFrequency resampling, volume,
 * headroom and mixbin volumes, Xbox ADPCM decoding against a reference
 * decoder written after the library's CImaAdpcmCodec (also on a real
 * sample file when one is given), positions and notifications, the C thunk
 * and C++ "this" aliases, 3D panning, stream packets (completion order,
 * status, completed size, callback and event delivery, starvation, Flush,
 * Discontinuity, Release), the wave file media object and effects data.
 *
 * Build (32-bit, like xbcompat itself; needs libsdl2-dev:i386):
 *   gcc -m32 -no-pie -fno-pie -std=gnu11 -Wall -Isrc \
 *       $(PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig pkg-config --cflags sdl2) \
 *       -o dsound_test tests/dsound_test.c \
 *       $(PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig pkg-config --libs sdl2) -lpthread -lm
 * Run:  SDL_AUDIODRIVER=dummy ./dsound_test [xbox-adpcm.wav]
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stddef.h>
#include <unistd.h>

#include "../src/hle/dsound.c"

/* ---- stubs for the rest of xbcompat ------------------------------------ */

int g_trace = 0;
FILE *g_log;
static int quiet;

void xlog(const char *fmt, ...)
{
    if (quiet) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("  log: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

void *pool_alloc(size_t size) { return calloc(1, size); }
void pool_free(void *p) { free(p); }
void prof_thread_start(void) {}
void install_fault_handlers(void) {}
void xbc_at_exit(void (*fn)(void)) { atexit(fn); }
NTSTATUS handle_close(HANDLE h) { (void)h; return 0xC0000008; }
NTSTATUS NTAPI NtOpenFile(HANDLE *h, ACCESS_MASK access, OBJECT_ATTRIBUTES *oa,
                          IO_STATUS_BLOCK *io, ULONG share, ULONG options)
{
    (void)h; (void)access; (void)oa; (void)io; (void)share; (void)options;
    return 0xC0000008; /* Tests use host files, not DVD handles. */
}
PVOID NTAPI MmAllocateContiguousMemoryEx(SIZE_T n, ULONG_PTR lo, ULONG_PTR hi, ULONG_PTR align, ULONG prot)
{
    (void)lo; (void)hi; (void)align; (void)prot;
    return calloc(1, n);
}
void NTAPI MmFreeContiguousMemory(PVOID p) { free(p); }
xthread *thread_adopt_host(const char *name) { (void)name; return NULL; }

/* Events are fake handles 1..63; NtSetEvent counts signals per handle. */
static int event_count[64];
static xobject fake_object;
xobject *handle_lookup(HANDLE h) { return (uintptr_t)h > 0 && (uintptr_t)h < 64 ? &fake_object : NULL; }
NTSTATUS NTAPI NtSetEvent(HANDLE h, LONG *prev)
{
    (void)prev;
    if ((uintptr_t)h < 64) event_count[(uintptr_t)h]++;
    return 0;
}

/* Guest paths are host paths here. */
NTSTATUS fs_translate(const OBJECT_ATTRIBUTES *oa, char *host, size_t hostlen, int *is_device)
{
    snprintf(host, hostlen, "%.*s", oa->ObjectName->Length, oa->ObjectName->Buffer);
    *is_device = 0;
    return 0;
}
NTSTATUS NTAPI NtReadFile(HANDLE f, HANDLE e, PVOID r, PVOID c, IO_STATUS_BLOCK *io, PVOID b, ULONG n,
                          LARGE_INTEGER *o)
{
    return 0xC0000008;   /* handle-based files are not used by the test */
}
NTSTATUS NTAPI NtWriteFile(HANDLE f, HANDLE e, PVOID r, PVOID c, IO_STATUS_BLOCK *io, PVOID b, ULONG n,
                           LARGE_INTEGER *o)
{
    return 0xC0000008;
}
NTSTATUS NTAPI NtQueryInformationFile(HANDLE f, IO_STATUS_BLOCK *io, PVOID b, ULONG n, ULONG cls)
{
    return 0xC0000008;
}

/* ---- test helpers --------------------------------------------------------- */

static int failures, checks;

#define CHECK(cond, ...) do { \
        checks++; \
        if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                       fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
    } while (0)

static int near(float a, float b, float tol) { return fabsf(a - b) <= tol; }

static float out[MIX_FRAMES * 2 * 64];

/* Mix n frames (a multiple of nothing in particular) into out. */
static void mix(int n)
{
    for (int done = 0; done < n;) {
        int k = n - done > MIX_FRAMES ? MIX_FRAMES : n - done;
        ds_mix_block(out + 2 * done, k);
        done += k;
    }
}

static WAVEFORMATEX pcm_format(int ch, int rate, int bits)
{
    WAVEFORMATEX w;
    XAudioCreatePcmFormat((uint16_t)ch, (DWORD)rate, (uint16_t)bits, &w);
    return w;
}

static uint32_t make_buffer(WAVEFORMATEX *w, DWORD flags)
{
    DSBUFFERDESC d = { sizeof(d), flags, 0, w, NULL, 0 };
    uint32_t p = 0;
    HRESULT hr = DirectSoundCreateBuffer(&d, &p);
    CHECK(hr == DS_OK && p, "DirectSoundCreateBuffer: %#x", (unsigned)hr);
    return p;
}

/* Unity gain for a 2D voice: no 2D headroom (SetHeadroom 0) and mixbin
   headroom 0 on the front bins. */
static void unity(uint32_t p)
{
    CHECK(Voice_SetHeadroom((void *)p, 0) == DS_OK, "SetHeadroom");
    CHECK(Voice_SetVolume((void *)p, 0) == DS_OK, "SetVolume");
}

/* ---- reference ADPCM codec, after CImaAdpcmCodec (common/imaadpcm.cpp) ----- */

static const short ref_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
    97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
    4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767
};
static const short ref_next[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static int ref_decode_sample(int enc, int pred, int step)
{
    long diff = step >> 3;
    if (enc & 4) diff += step;
    if (enc & 2) diff += step >> 1;
    if (enc & 1) diff += step >> 2;
    if (enc & 8) diff = -diff;
    long s = pred + diff;
    if ((long)(short)s != s) s = s < -32768 ? -32768 : 32767;
    return (int)s;
}

static int ref_next_index(int enc, int idx)
{
    idx += ref_next[enc];
    return idx < 0 ? 0 : idx > 88 ? 88 : idx;
}

/* DecodeM16 / DecodeS16 for one 64-sample block. */
static int ref_decode_block(const uint8_t *src, int ch, short *dst)
{
    int pred[2], idx[2];
    for (int c = 0; c < ch; c++) {
        uint32_t h; memcpy(&h, src + 4 * c, 4);
        pred[c] = (short)(h & 0xFFFF);
        idx[c] = (uint8_t)(h >> 16);
        if (idx[c] > 88) return 0;
        dst[c] = (short)pred[c];
    }
    const uint8_t *b = src + 4 * ch;
    short *o = dst + ch;
    int left = 63;
    if (ch == 1) {
        while (left) {
            uint8_t v = *b++;
            pred[0] = ref_decode_sample(v & 15, pred[0], ref_step[idx[0]]);
            idx[0] = ref_next_index(v & 15, idx[0]);
            *o++ = (short)pred[0];
            if (--left) {
                pred[0] = ref_decode_sample(v >> 4, pred[0], ref_step[idx[0]]);
                idx[0] = ref_next_index(v >> 4, idx[0]);
                *o++ = (short)pred[0];
                left--;
            }
        }
    } else {
        while (left) {
            uint32_t l, r;
            memcpy(&l, b, 4); memcpy(&r, b + 4, 4); b += 8;
            int n = left < 8 ? left : 8;
            for (int i = 0; i < n; i++) {
                pred[0] = ref_decode_sample(l & 15, pred[0], ref_step[idx[0]]);
                idx[0] = ref_next_index(l & 15, idx[0]);
                pred[1] = ref_decode_sample(r & 15, pred[1], ref_step[idx[1]]);
                idx[1] = ref_next_index(r & 15, idx[1]);
                *o++ = (short)pred[0]; *o++ = (short)pred[1];
                l >>= 4; r >>= 4;
            }
            left -= n;
        }
    }
    return 1;
}

/* EncodeSample, tracking the decoder's predictor so the data round-trips. */
static int ref_encode_sample(int in, int *pred, int *idx)
{
    int step = ref_step[*idx], d = in - *pred, enc = 0;
    if (d < 0) { enc = 8; d = -d; }
    if (d >= step) { enc |= 4; d -= step; }
    step >>= 1;
    if (d >= step) { enc |= 2; d -= step; }
    step >>= 1;
    if (d >= step) enc |= 1;
    *pred = ref_decode_sample(enc, *pred, ref_step[*idx]);
    *idx = ref_next_index(enc, *idx);
    return enc;
}

/* Encode nblocks of 64 frames from interleaved 16-bit PCM. */
static void ref_encode(const short *pcm, int ch, int nblocks, uint8_t *dst)
{
    int idx[2] = { 0, 0 };
    for (int blk = 0; blk < nblocks; blk++, pcm += 64 * ch, dst += 36 * ch) {
        int pred[2];
        for (int c = 0; c < ch; c++) {
            pred[c] = pcm[c];
            uint32_t h = (uint16_t)pcm[c] | (uint32_t)idx[c] << 16;
            memcpy(dst + 4 * c, &h, 4);
        }
        uint8_t *b = dst + 4 * ch;
        memset(b, 0, 32 * ch);
        for (int k = 1; k < 64; k++) {
            for (int c = 0; c < ch; c++) {
                int n = ref_encode_sample(pcm[k * ch + c], &pred[c], &idx[c]);
                int nib = k - 1;
                if (ch == 1) b[nib >> 1] |= (uint8_t)(n << ((nib & 1) * 4));
                else b[(nib / 8) * 8 + c * 4 + (nib % 8) / 2] |= (uint8_t)(n << ((nib & 1) * 4));
            }
        }
    }
}

/* ---- tests ------------------------------------------------------------------ */

static void test_formats(void)
{
    CHECK(XAudioCalculatePitch(48000) == 0, "pitch 48k");
    CHECK(XAudioCalculatePitch(24000) == -4096, "pitch 24k = %d", (int)XAudioCalculatePitch(24000));
    CHECK(XAudioCalculatePitch(96000) == 4096, "pitch 96k");
    CHECK(XAudioCalculatePitch(44100) == -501, "pitch 44.1k = %d", (int)XAudioCalculatePitch(44100));
    WAVEFORMATEX w = pcm_format(2, 22050, 16);
    CHECK(w.wFormatTag == 1 && w.nChannels == 2 && w.nBlockAlign == 4 && w.nAvgBytesPerSec == 88200 &&
          w.cbSize == 0, "XAudioCreatePcmFormat");
    XBOXADPCMWAVEFORMAT a;
    XAudioCreateAdpcmFormat(2, 44100, &a);
    CHECK(a.wfx.wFormatTag == 0x69 && a.wfx.nBlockAlign == 72 && a.wfx.wBitsPerSample == 4 &&
          a.wfx.cbSize == 2 && a.wSamplesPerBlock == 64 && a.wfx.nAvgBytesPerSec == 44100 / 64 * 36,
          "XAudioCreateAdpcmFormat");
    struct ds_fmt f;
    CHECK(fmt_parse(&a.wfx, &f, NULL) == DS_OK && f.adpcm && f.unit_bytes == 72 && f.unit_frames == 64,
          "ADPCM format parse");
    w.nBlockAlign = 3;
    CHECK(fmt_parse(&w, &f, NULL) != DS_OK, "bad block align rejected");
}

static void test_object_aliases(void)
{
    uint32_t ds = 0;
    CHECK(DirectSoundCreate(NULL, &ds, NULL) == DS_OK && ds, "DirectSoundCreate");
    uint32_t ds2 = 0;
    CHECK(DirectSoundCreate(NULL, &ds2, NULL) == DS_OK && ds2 == ds, "singleton");
    /* The library's CRefCount layout: {vtable, refs} at p-8 with AddRef/Release in slots 1/2. */
    struct ds_refhdr *h = (struct ds_refhdr *)(ds - 8);
    CHECK(h->refs == 2, "refs %u", h->refs);
    ULONG (NTAPI *addref)(void *) = (ULONG (NTAPI *)(void *))h->vtbl[1];
    ULONG (NTAPI *release)(void *) = (ULONG (NTAPI *)(void *))h->vtbl[2];
    CHECK(addref(h) == 3, "vtable AddRef");
    CHECK(release(h) == 2, "vtable Release");
    DSCAPS caps;
    CHECK(DS_GetCaps((void *)ds, &caps) == DS_OK && caps.dwFree2DBuffers == 256, "GetCaps via p");
    CHECK(DS_GetCaps((void *)(ds - 0x10), &caps) == DS_OK, "GetCaps via debug this (p-0x10)");
    CHECK(DS_GetCaps((void *)(ds - 8), &caps) == DS_OK, "GetCaps via release this (p-8)");
    CHECK(DS_GetCaps((void *)(ds + 4), &caps) != DS_OK, "GetCaps via a wrong pointer fails");

    WAVEFORMATEX w = pcm_format(1, 48000, 16);
    uint32_t b = make_buffer(&w, 0);
    CHECK(Voice_SetVolume((void *)b, -100) == DS_OK, "SetVolume via thunk pointer");
    CHECK(Voice_SetVolume((void *)(b - 0x1C), -200) == DS_OK, "SetVolume via release this / voice");
    CHECK(Voice_SetVolume((void *)(b - 0x24), -300) == DS_OK, "SetVolume via debug this");
    CHECK(find_buffer((void *)b)->v.volume == -300 - 600, "volume includes 2D headroom: %d",
          (int)find_buffer((void *)b)->v.volume);
    CHECK(DS_GetCaps((void *)ds, &caps) == DS_OK && caps.dwFree2DBuffers == 255, "GetCaps counts the buffer");
    CHECK(Obj_Release((void *)b) == 0, "buffer released");
    CHECK(Obj_Release((void *)ds) == 1 && Obj_Release((void *)ds) == 0, "DirectSound released");
    CHECK(g.ds == NULL, "singleton destroyed");
}

static void test_playback(void)
{
    WAVEFORMATEX w = pcm_format(1, 48000, 16);
    short *pcm = calloc(1000, 2);
    for (int i = 0; i < 1000; i++) pcm[i] = 16384;           /* 0.5 */
    uint32_t b = make_buffer(&w, 0);
    unity(b);
    CHECK(Buf_SetBufferData((void *)b, pcm, 2000) == DS_OK, "SetBufferData");
    DWORD st = 99;
    CHECK(Buf_GetStatus((void *)b, &st) == DS_OK && st == 0, "stopped status %#x", st);
    CHECK(Buf_Play((void *)b, 0, 0, 0) == DS_OK, "Play");
    CHECK(Buf_GetStatus((void *)b, &st) == DS_OK && st == DSBSTATUS_PLAYING, "playing status %#x", st);
    mix(512);
    /* mono default mixbins FL+FR, mixbin headroom 1 (x0.5): 0.5 * 0.5 = 0.25 on each side */
    CHECK(near(out[0], 0.25f, 1e-4f) && near(out[1], 0.25f, 1e-4f), "mono to stereo: %f %f", out[0], out[1]);
    DWORD play = 0, write = 0;
    CHECK(Buf_GetCurrentPosition((void *)b, &play, &write) == DS_OK && play == 1024 && write == 1024 + 64,
          "cursor %u / %u", play, write);
    mix(1024);
    CHECK(near(out[2 * 487], 0.25f, 1e-4f) && out[2 * 488] == 0 && out[2 * 1023] == 0,
          "one-shot ends after 1000 frames: %f %f", out[2 * 487], out[2 * 488]);
    CHECK(Buf_GetStatus((void *)b, &st) == DS_OK && st == 0, "status after the end %#x", st);

    /* A second buffer mixes in: lower mixbin headroom to 0 for unity. */
    uint32_t ds = 0;
    DirectSoundCreate(NULL, &ds, NULL);
    DS_SetMixBinHeadroom((void *)ds, 0, 0);
    DS_SetMixBinHeadroom((void *)ds, 1, 0);
    short *pcm2 = calloc(1000, 2);
    for (int i = 0; i < 1000; i++) pcm2[i] = -8192;          /* -0.25 */
    uint32_t b2 = make_buffer(&w, 0);
    unity(b2);
    Buf_SetBufferData((void *)b2, pcm2, 2000);
    Buf_Play((void *)b, 0, 0, DSBPLAY_FROMSTART);
    Buf_Play((void *)b2, 0, 0, 0);
    mix(256);
    CHECK(near(out[10], 0.25f, 1e-4f) && near(out[11], 0.25f, 1e-4f), "two voices sum: %f", out[10]);

    /* Volume: -600 = 10^(-0.3) */
    Voice_SetVolume((void *)b2, -600);
    mix(256);
    CHECK(near(out[10], 0.5f - 0.25f * powf(10, -0.3f), 1e-4f), "volume -6 dB: %f", out[10]);
    /* Mixbin volumes: right bin of buffer 1 at -10000 (silent) */
    DSMIXBINVOLUMEPAIR pr = { 1, -10000 };
    DSMIXBINS mb = { 1, &pr };
    CHECK(Voice_SetMixBinVolumes((void *)b, &mb) == DS_OK, "SetMixBinVolumes");
    mix(256);
    CHECK(near(out[11], -0.25f * powf(10, -0.3f), 1e-4f) && near(out[10], 0.5f - 0.25f * powf(10, -0.3f), 1e-4f),
          "mixbin volume: L %f R %f", out[10], out[11]);
    DSMIXBINVOLUMEPAIR bad = { 5, 0 };
    DSMIXBINS mbad = { 1, &bad };
    CHECK(Voice_SetMixBinVolumes((void *)b, &mbad) == DSERR_INVALIDPARAM, "unassigned mixbin rejected");

    /* Stop caches the cursor; Play resumes there. */
    CHECK(Buf_Stop((void *)b) == DS_OK, "Stop");
    CHECK(Buf_GetCurrentPosition((void *)b, &play, &write) == DS_OK && play == 2 * 768 && write == play,
          "stopped cursor %u", play);
    CHECK(Buf_SetCurrentPosition((void *)b, 1990) == DS_OK, "SetCurrentPosition");
    Buf_Stop((void *)b2);
    Buf_Play((void *)b, 0, 0, 0);
    mix(16);
    CHECK(near(out[8], 0.5f, 1e-4f) && out[2 * 5] == 0, "resume from cursor: %f %f", out[8], out[10]);

    Obj_Release((void *)b); Obj_Release((void *)b2);
    DS_SetMixBinHeadroom((void *)ds, 0, 1);
    DS_SetMixBinHeadroom((void *)ds, 1, 1);
    Obj_Release((void *)ds);
    free(pcm); free(pcm2);
}

static void test_legacy_mixbin_headroom(void)
{
    uint32_t ds = 0;
    DirectSoundCreate(NULL, &ds, NULL);
    WAVEFORMATEX w = pcm_format(1, 48000, 16);
    short pcm[64];
    for (unsigned i = 0; i < 64; i++) pcm[i] = 16384;
    uint32_t b = make_buffer(&w, 0);
    Voice_SetHeadroom((void *)b, 1200); /* Dashboard keeps 12 dB voice headroom. */
    Buf_SetBufferData((void *)b, pcm, sizeof(pcm));
    Buf_Play((void *)b, 0, 0, DSBPLAY_LOOPING);
    float full = 0.5f * powf(10.0f, -0.6f);
    mix(16);
    CHECK(near(out[0], full * 0.5f, 1e-5f), "default mix-bin headroom");
    CHECK(DS_SetMixBinHeadroom((void *)ds, 0x7FFFFFFF, 0) == DS_OK, "Dashboard all-bin mask");
    mix(16);
    CHECK(near(out[0], full, 1e-5f) && near(out[1], full, 1e-5f),
          "all-bin headroom removes only the extra 6 dB: %f %f", out[0], out[1]);
    CHECK(DS_SetMixBinHeadroom((void *)ds, 1, 1) == DS_OK, "modern bin 1 is right");
    mix(16);
    CHECK(near(out[0], full, 1e-5f) && near(out[1], full * 0.5f, 1e-5f), "index semantics preserved");
    CHECK(DS_SetMixBinHeadroom_v1((void *)ds, 1, 1) == DS_OK, "legacy mask 1 is left");
    mix(16);
    CHECK(near(out[0], full * 0.5f, 1e-5f) && near(out[1], full * 0.5f, 1e-5f), "v1 mask semantics");
    CHECK(DS_SetMixBinHeadroom((void *)ds, 32, 0) == DSERR_INVALIDPARAM, "invalid modern bin");
    CHECK(DS_SetMixBinHeadroom((void *)ds, 0x7FFFFFFF, 8) == DSERR_INVALIDPARAM, "invalid mask headroom");
    DS_SetMixBinHeadroom((void *)ds, 0x7FFFFFFF, 1);
    Obj_Release((void *)b);
    Obj_Release((void *)ds);
}

static void test_looping_and_regions(void)
{
    WAVEFORMATEX w = pcm_format(1, 48000, 16);
    short pcm[100];
    for (int i = 0; i < 100; i++) pcm[i] = (short)(i * 100);
    uint32_t b = make_buffer(&w, 0);
    unity(b);
    Buf_SetBufferData((void *)b, pcm, sizeof(pcm));
    CHECK(Buf_Play((void *)b, 0, 0, DSBPLAY_LOOPING) == DS_OK, "Play looping");
    mix(1024);
    int ok = 1;
    for (int i = 0; i < 1024; i++)
        if (!near(out[2 * i], 0.5f * pcm[i % 100] / 32768.0f, 1e-5f)) { ok = 0; break; }
    CHECK(ok, "loop over the whole buffer");
    DWORD st;
    Buf_GetStatus((void *)b, &st);
    CHECK(st == (DSBSTATUS_PLAYING | DSBSTATUS_LOOPING), "looping status %#x", st);

    /* Loop region [40, 60) frames: play continues to 60, then cycles 40..59. */
    Buf_Stop((void *)b);
    CHECK(Buf_SetLoopRegion((void *)b, 80, 40) == DS_OK, "SetLoopRegion");
    CHECK(Buf_SetLoopRegion((void *)b, 81, 40) == DSERR_INVALIDPARAM, "unaligned loop rejected");
    CHECK(Buf_SetLoopRegion((void *)b, 160, 80) == DSERR_INVALIDCALL, "loop past the end rejected");
    Buf_Play((void *)b, 0, 0, DSBPLAY_LOOPING | DSBPLAY_FROMSTART);
    mix(200);
    ok = 1;
    for (int i = 0; i < 200; i++) {
        int src = i < 60 ? i : 40 + (i - 40) % 20;
        if (!near(out[2 * i], 0.5f * pcm[src] / 32768.0f, 1e-5f)) { ok = 0; fprintf(stderr, "  at %d\n", i); break; }
    }
    CHECK(ok, "loop region");
    DWORD play;
    Buf_GetCurrentPosition((void *)b, &play, NULL);
    CHECK(play == 2 * (40 + (200 - 40) % 20), "cursor inside the loop: %u", play);

    /* Play region [20, 70) frames, one shot; loop region resets to it. */
    Buf_Stop((void *)b);
    CHECK(Buf_SetPlayRegion((void *)b, 40, 100) == DS_OK, "SetPlayRegion");
    Buf_Play((void *)b, 0, 0, 0);
    mix(64);
    CHECK(near(out[0], 0.5f * pcm[20] / 32768.0f, 1e-5f) && near(out[2 * 49], 0.5f * pcm[69] / 32768.0f, 1e-5f) &&
          out[2 * 50] == 0, "play region");

    /* StopEx(RELEASEWAVEFORM) on a looping voice plays out to the end of the region. */
    Buf_SetPlayRegion((void *)b, 0, 0);
    Buf_SetLoopRegion((void *)b, 0, 40);
    Buf_Play((void *)b, 0, 0, DSBPLAY_LOOPING);
    mix(30);
    Buf_StopEx((void *)b, 0, 0, DSBSTOPEX_ENVELOPE | DSBSTOPEX_RELEASEWAVEFORM);
    mix(100);
    /* 30 frames = one lap of 20 and 10 more; then frames 10..99 play once. */
    CHECK(near(out[2 * 89], 0.5f * pcm[99] / 32768.0f, 1e-5f) && out[2 * 90] == 0,
          "release waveform: %f %f", out[2 * 89], out[2 * 90]);
    Obj_Release((void *)b);
}

static void test_pitch(void)
{
    short pcm[400];
    for (int i = 0; i < 400; i++) pcm[i] = (short)(i * 50);
    WAVEFORMATEX w = pcm_format(1, 24000, 16);       /* half rate: 0.5 source frames per output frame */
    uint32_t b = make_buffer(&w, 0);
    unity(b);
    Buf_SetBufferData((void *)b, pcm, sizeof(pcm));
    Buf_Play((void *)b, 0, 0, 0);
    mix(512);
    int ok = 1;
    for (int i = 0; i < 512 && ok; i++) {
        float src = (i / 2) * 50 + (i & 1) * 25;     /* linear interpolation */
        if (!near(out[2 * i], 0.5f * src / 32768.0f, 1e-4f)) { ok = 0; fprintf(stderr, "  at %d: %f\n", i, out[2 * i]); }
    }
    CHECK(ok, "24 kHz source resampled to 48 kHz");
    DWORD play;
    Buf_GetCurrentPosition((void *)b, &play, NULL);
    CHECK(play == 2 * 256, "cursor advanced by 256 source frames: %u", play);

    /* SetFrequency(96000): two source frames per output frame. */
    CHECK(Voice_SetFrequency((void *)b, 96000) == DS_OK, "SetFrequency");
    Buf_Play((void *)b, 0, 0, DSBPLAY_FROMSTART);
    mix(256);
    CHECK(near(out[2 * 100], 0.5f * pcm[200] / 32768.0f, 1e-4f) && out[2 * 200] == 0,
          "96 kHz plays twice as fast: %f, %f", out[2 * 100], out[2 * 200]);
    /* SetFrequency(0) restores the format rate; SetPitch(0) is 48 kHz. */
    Voice_SetFrequency((void *)b, 0);
    CHECK(find_buffer((void *)b)->v.pitch == -4096, "SetFrequency(0)");
    CHECK(Voice_SetPitch((void *)b, 0) == DS_OK && find_buffer((void *)b)->v.pitch == 0, "SetPitch");
    CHECK(Voice_SetPitch((void *)b, 9000) == DSERR_INVALIDPARAM, "pitch range");
    CHECK(Voice_SetFrequency((void *)b, 100) == DSERR_INVALIDPARAM, "frequency range");
    Obj_Release((void *)b);
}

static void test_adpcm(const char *wavpath)
{
    /* Encode a stereo sine sweep, decode with dsound.c and with the reference. */
    enum { NB = 40 };
    short *pcm = malloc(NB * 64 * 2 * sizeof(short));
    for (int i = 0; i < NB * 64; i++) {
        pcm[2 * i] = (short)(20000 * sin(i * 0.05));
        pcm[2 * i + 1] = (short)(12000 * sin(i * 0.013 + 1));
    }
    uint8_t *adpcm = malloc(NB * 72);
    ref_encode(pcm, 2, NB, adpcm);
    short a[128], r[128];
    int same = 1;
    double err = 0;     /* RMS error of the round trip, after the first block */
    for (int blk = 0; blk < NB; blk++) {
        ds_adpcm_decode_block(adpcm + blk * 72, 2, a);
        ref_decode_block(adpcm + blk * 72, 2, r);
        if (memcmp(a, r, sizeof(a))) same = 0;
        for (int k = 0; blk && k < 128; k++) err += (a[k] - pcm[blk * 128 + k]) * (double)(a[k] - pcm[blk * 128 + k]);
    }
    err = sqrt(err / ((NB - 1) * 128));
    CHECK(same, "stereo ADPCM decode matches the reference");
    CHECK(err < 400, "stereo ADPCM round trip RMS error %g", err);

    uint8_t *mono = malloc(NB * 36);
    short *mpcm = malloc(NB * 64 * sizeof(short));
    for (int i = 0; i < NB * 64; i++) mpcm[i] = (short)(25000 * sin(i * 0.07));
    ref_encode(mpcm, 1, NB, mono);
    same = 1; err = 0;
    for (int blk = 0; blk < NB; blk++) {
        ds_adpcm_decode_block(mono + blk * 36, 1, a);
        ref_decode_block(mono + blk * 36, 1, r);
        if (memcmp(a, r, 64 * sizeof(short))) same = 0;
        for (int k = 0; blk && k < 64; k++) err += (a[k] - mpcm[blk * 64 + k]) * (double)(a[k] - mpcm[blk * 64 + k]);
    }
    err = sqrt(err / ((NB - 1) * 64));
    CHECK(same, "mono ADPCM decode matches the reference");
    CHECK(err < 400, "mono ADPCM round trip RMS error %g", err);

    /* Corrupt step index: the block decodes to silence. */
    uint8_t badblk[36] = { 0x34, 0x12, 90, 0 };
    CHECK(ds_adpcm_decode_block(badblk, 1, a) == 0 && a[0] == 0, "invalid step index");

    /* A buffer of ADPCM plays the decoded samples (48 kHz, unity). */
    XBOXADPCMWAVEFORMAT f;
    XAudioCreateAdpcmFormat(1, 48000, &f);
    uint32_t b = make_buffer(&f.wfx, 0);
    unity(b);
    CHECK(Buf_SetBufferData((void *)b, mono, NB * 36) == DS_OK, "ADPCM SetBufferData");
    CHECK(Buf_SetBufferData((void *)b, mono, 50) == DSERR_INVALIDPARAM, "ADPCM size must be whole blocks");
    Buf_Play((void *)b, 0, 0, 0);
    mix(1000);
    int ok = 1;
    for (int i = 0; i < 1000 && ok; i++) {
        short ref[64];
        ref_decode_block(mono + (i / 64) * 36, 1, ref);
        if (!near(out[2 * i], 0.5f * ref[i % 64] / 32768.0f, 1e-5f)) { ok = 0; fprintf(stderr, "  at %d\n", i); }
    }
    CHECK(ok, "ADPCM buffer playback");
    DWORD play, write;
    Buf_GetCurrentPosition((void *)b, &play, &write);
    CHECK(play == 1000 / 64 * 36 && write == play + 36, "ADPCM cursor in blocks: %u %u", play, write);
    Obj_Release((void *)b);

    /* A real Xbox ADPCM file (FileStream's becky_xbadpcm.wav), when given. */
    if (wavpath) {
        void *xmo = NULL;
        const WAVEFORMATEX *wf = NULL;
        HRESULT hr = XWaveFileCreateMediaObject(wavpath, &wf, &xmo);
        CHECK(hr == DS_OK && wf && wf->wFormatTag == 0x69, "open %s", wavpath);
        if (hr == DS_OK) {
            int ch = wf->nChannels;
            DWORD len = 0;
            Wave_GetLength(xmo, &len);
            uint8_t *data = malloc(len);
            DWORD got = 0, status = 1;
            XMEDIAPACKET xp = { data, len, &got, &status, NULL, NULL };
            Wave_Process(xmo, NULL, &xp);
            CHECK(status == 0 && got == len / (36u * ch) * 36u * ch, "wave XMO read %u of %u", got, len);
            DWORD blocks = got / (36 * ch), mismatched = 0, bad = 0;
            for (DWORD i = 0; i < blocks; i++) {
                short x[128], y[128];
                int okx = ds_adpcm_decode_block(data + i * 36 * ch, ch, x);
                int oky = ref_decode_block(data + i * 36 * ch, ch, y);
                if (!oky) bad++;
                else if (!okx || memcmp(x, y, 64 * ch * sizeof(short))) mismatched++;
            }
            CHECK(!mismatched && !bad, "%u real blocks: %u mismatched, %u invalid", blocks, mismatched, bad);
            printf("  decoded %u blocks of %s against the reference\n", blocks, wavpath);
            free(data);
            Wave_Release(xmo);
        }
    }
    free(pcm); free(adpcm); free(mono); free(mpcm);
}

static void test_notifications(void)
{
    WAVEFORMATEX w = pcm_format(1, 48000, 16);
    short pcm[1000] = { 0 };
    uint32_t b = make_buffer(&w, DSBCAPS_CTRLPOSITIONNOTIFY);
    Buf_SetBufferData((void *)b, pcm, sizeof(pcm));
    DSBPOSITIONNOTIFY n[3] = { { 1000, (HANDLE)2 }, { 200, (HANDLE)1 }, { DSBPN_OFFSETSTOP, (HANDLE)3 } };
    CHECK(Buf_SetNotificationPositions((void *)b, 3, n) == DS_OK, "SetNotificationPositions");
    DSBPOSITIONNOTIFY badn = { 201, (HANDLE)1 };
    CHECK(Buf_SetNotificationPositions((void *)b, 1, &badn) == DSERR_INVALIDPARAM, "unaligned offset rejected");
    memset(event_count, 0, sizeof(event_count));
    Buf_Play((void *)b, 0, 0, 0);
    mix(300);
    CHECK(event_count[1] == 1 && event_count[2] == 0 && event_count[3] == 0, "offset 100 frames: %d %d %d",
          event_count[1], event_count[2], event_count[3]);
    mix(800);
    CHECK(event_count[1] == 1 && event_count[2] == 1 && event_count[3] == 1, "offset 500 and stop: %d %d %d",
          event_count[1], event_count[2], event_count[3]);
    /* Looping: every lap fires both offsets; Stop fires OFFSETSTOP. */
    memset(event_count, 0, sizeof(event_count));
    Buf_Play((void *)b, 0, 0, DSBPLAY_LOOPING | DSBPLAY_FROMSTART);
    mix(2600);
    CHECK(event_count[1] == 3 && event_count[2] == 3 && event_count[3] == 0, "looping laps: %d %d %d",
          event_count[1], event_count[2], event_count[3]);
    Buf_Stop((void *)b);
    CHECK(event_count[3] == 1, "Stop signals OFFSETSTOP");
    uint32_t nb = make_buffer(&w, 0);
    CHECK(Buf_SetNotificationPositions((void *)nb, 1, n) == DSERR_CONTROLUNAVAIL, "needs CTRLPOSITIONNOTIFY");
    Obj_Release((void *)nb);
    Obj_Release((void *)b);
}

static void test_lock(void)
{
    WAVEFORMATEX w = pcm_format(2, 48000, 16);
    DSBUFFERDESC d = { sizeof(d), 0, 4000, &w, NULL, 0 };
    uint32_t b = 0;
    CHECK(DirectSoundCreateBuffer(&d, &b) == DS_OK, "buffer with internal memory");
    void *p1, *p2; DWORD n1, n2;
    CHECK(Buf_Lock((void *)b, 3000, 2000, &p1, &n1, &p2, &n2, 0) == DS_OK && n1 == 1000 && n2 == 1000 &&
          p2 == find_buffer((void *)b)->data && (uint8_t *)p1 == find_buffer((void *)b)->data + 3000, "wrapping Lock");
    CHECK(Buf_Lock((void *)b, 0, 0, &p1, &n1, NULL, NULL, DSBLOCK_ENTIREBUFFER) == DS_OK && n1 == 4000, "ENTIREBUFFER");
    CHECK(Buf_Lock((void *)b, 2, 8, &p1, &n1, NULL, NULL, 0) == DSERR_INVALIDPARAM, "unaligned Lock rejected");
    short *s = p1;
    for (int i = 0; i < 1000; i++) { s[2 * i] = 8192; s[2 * i + 1] = -8192; }
    Buf_Unlock((void *)b, p1, n1, NULL, 0);
    unity(b);
    Buf_Play((void *)b, 0, 0, 0);
    mix(8);
    CHECK(near(out[0], 0.125f, 1e-5f) && near(out[1], -0.125f, 1e-5f), "stereo channels to FL/FR: %f %f", out[0], out[1]);
    Obj_Release((void *)b);
}

static void test_3d(void)
{
    uint32_t ds = 0;
    DirectSoundCreate(NULL, &ds, NULL);
    DirectSoundUsePan3D();
    WAVEFORMATEX w = pcm_format(1, 48000, 16);
    short pcm[2000];
    for (int i = 0; i < 2000; i++) pcm[i] = 16384;
    uint32_t b = make_buffer(&w, DSBCAPS_CTRL3D);
    CHECK(find_buffer((void *)b)->v.headroom == 0, "3D voices have no headroom");
    Buf_SetBufferData((void *)b, pcm, sizeof(pcm));
    Buf_Play((void *)b, 0, 0, DSBPLAY_LOOPING);
    /* Source to the listener's right (+x), 1 m away: louder on the right. */
    CHECK(Voice_SetPosition((void *)b, 1, 0, 0, 0) == DS_OK, "SetPosition");
    mix(64);
    float l = out[2 * 10], r = out[2 * 10 + 1];
    CHECK(r > 2 * l && r > 0.1f, "pan right: L %f R %f", l, r);
    /* Deferred: nothing changes until CommitDeferredSettings. */
    Voice_SetPosition((void *)b, -1, 0, 0, DS3D_DEFERRED);
    mix(64);
    CHECK(near(out[2 * 10 + 1], r, 1e-6f), "deferred position not applied yet");
    DS_CommitDeferredSettings((void *)ds);
    mix(64);
    CHECK(out[2 * 10] > 2 * out[2 * 10 + 1], "pan left after commit: L %f R %f", out[2 * 10], out[2 * 10 + 1]);
    /* Distance: 10 m with min distance 1 attenuates by 20 dB (rolloff 1). */
    float near_l = out[2 * 10];
    Voice_SetPosition((void *)b, -10, 0, 0, 0);
    mix(64);
    CHECK(near(out[2 * 10] / near_l, 0.1f, 0.01f), "distance rolloff ratio %f", out[2 * 10] / near_l);
    /* 3D setters on a 2D voice are refused. */
    uint32_t b2 = make_buffer(&w, 0);
    CHECK(Voice_SetPosition((void *)b2, 0, 0, 0, 0) == DSERR_CONTROLUNAVAIL, "2D voice refuses 3D");
    Obj_Release((void *)b2);
    Obj_Release((void *)b);
    Obj_Release((void *)ds);
}

static void test_submix(void)
{
    /* A MIXIN buffer plays whatever the voices routed to it produce. */
    DSBUFFERDESC md = { sizeof(md), DSBCAPS_MIXIN, 0, NULL, NULL, 0 };
    uint32_t mix_buf = 0;
    CHECK(DirectSoundCreateBuffer(&md, &mix_buf) == DS_OK, "MIXIN buffer");
    WAVEFORMATEX w = pcm_format(1, 48000, 16);
    short pcm[1000];
    for (int i = 0; i < 1000; i++) pcm[i] = 16384;
    DSMIXBINS none = { 0, NULL };
    DSBUFFERDESC d = { sizeof(d), 0, 0, &w, &none, 0 };     /* no mixbins: only the submix hears it */
    uint32_t b = 0;
    CHECK(DirectSoundCreateBuffer(&d, &b) == DS_OK, "source buffer without mixbins");
    unity(b);
    Buf_SetBufferData((void *)b, pcm, sizeof(pcm));
    Buf_Play((void *)b, 0, 0, DSBPLAY_LOOPING);
    mix(64);
    CHECK(out[20] == 0 && out[21] == 0, "silent without an output buffer");
    CHECK(Voice_SetOutputBuffer((void *)b, (void *)mix_buf) == DS_OK, "SetOutputBuffer");
    CHECK(Voice_SetOutputBuffer((void *)b, (void *)b) == DSERR_INVALIDPARAM, "output must be MIXIN/FXIN");
    mix(64);
    /* source 0.5 -> SUBMIX bin (headroom 0) -> submix voice on FL/FR (mixbin headroom 1) */
    CHECK(near(out[20], 0.25f, 1e-4f) && near(out[21], 0.25f, 1e-4f), "submixed: %f %f", out[20], out[21]);
    Voice_SetVolume((void *)mix_buf, -600);
    mix(64);
    CHECK(near(out[20], 0.25f * powf(10, -0.3f), 1e-4f), "submix voice volume: %f", out[20]);
    /* The source holds a reference on its output buffer (SetOutputBuffer
       AddRefs it), so the title's Release does not cut the route. */
    CHECK(find_buffer((void *)mix_buf)->hdr.refs == 2, "SetOutputBuffer takes a reference");
    CHECK(Obj_Release((void *)mix_buf) == 1, "the source keeps the output buffer");
    mix(64);
    CHECK(near(out[20], 0.25f * powf(10, -0.3f), 1e-4f), "still routed after the title's Release: %f", out[20]);
    CHECK(Voice_SetOutputBuffer((void *)b, NULL) == DS_OK, "disconnect");
    LOCK();
    int gone = find_buffer((void *)mix_buf) == NULL;
    UNLOCK();
    CHECK(gone, "disconnecting drops the last reference");
    mix(64);
    CHECK(out[20] == 0 && out[21] == 0, "disconnected: the submix bin is gone, no mixbins left");
    Obj_Release((void *)b);
}

/* SetOutputBuffer replaces the voice's mixbins with the submix input bin
   (the voice is not heard directly any more); SetMixBins afterwards keeps
   that bin. */
static void test_submix_routing(void)
{
    DSBUFFERDESC md = { sizeof(md), DSBCAPS_MIXIN, 0, NULL, NULL, 0 };
    uint32_t mix_buf = 0;
    CHECK(DirectSoundCreateBuffer(&md, &mix_buf) == DS_OK, "MIXIN buffer");
    WAVEFORMATEX w = pcm_format(1, 48000, 16);
    short pcm[1000];
    for (int i = 0; i < 1000; i++) pcm[i] = 16384;
    uint32_t b = make_buffer(&w, 0);                 /* default mixbins FL+FR */
    unity(b);
    Buf_SetBufferData((void *)b, pcm, sizeof(pcm));
    Buf_Play((void *)b, 0, 0, DSBPLAY_LOOPING);
    mix(64);
    CHECK(near(out[20], 0.25f, 1e-4f) && near(out[21], 0.25f, 1e-4f), "direct: %f %f", out[20], out[21]);
    CHECK(Voice_SetOutputBuffer((void *)b, (void *)mix_buf) == DS_OK, "SetOutputBuffer");
    struct ds_voice *v = &find_buffer((void *)b)->v;
    CHECK(v->nbins == 1 && v->bins[0] == DSMIXBIN_SUBMIX, "mixbins replaced by the submix bin (%u)", v->nbins);
    mix(64);
    CHECK(near(out[20], 0.25f, 1e-4f) && near(out[21], 0.25f, 1e-4f), "only through the submix: %f %f",
          out[20], out[21]);
    DSMIXBINVOLUMEPAIR fl = { 0, 0 };
    DSMIXBINS mb = { 1, &fl };
    CHECK(Voice_SetMixBins((void *)b, &mb) == DS_OK, "SetMixBins with an output buffer");
    CHECK(v->nbins == 2 && v->bins[0] == 0 && v->bins[1] == DSMIXBIN_SUBMIX, "submix bin kept");
    mix(64);
    CHECK(near(out[20], 0.5f, 1e-4f) && near(out[21], 0.25f, 1e-4f), "direct FL + submix: %f %f",
          out[20], out[21]);
    Obj_Release((void *)mix_buf);
    Obj_Release((void *)b);                          /* frees the submix buffer too */
    LOCK();
    int gone = find_buffer((void *)mix_buf) == NULL;
    UNLOCK();
    CHECK(gone, "the source's release drops its reference on the output buffer");
}

/* Channel to mixbin slot: a 4-channel voice is two stereo hardware voices,
   each with half of the slots, left channel on the even slots. */
static void test_channel_slots(void)
{
    WAVEFORMATEX w = pcm_format(4, 48000, 16);
    uint32_t b = make_buffer(&w, 0);
    struct ds_voice *v = &find_buffer((void *)b)->v;
    CHECK(v->nbins == 4 && v->bins[2] == 4 && v->bins[3] == 5, "4-channel default mixbins");
    int ok = voice_slot_channel(v, 0) == 0 && voice_slot_channel(v, 1) == 1 &&
             voice_slot_channel(v, 2) == 2 && voice_slot_channel(v, 3) == 3;
    CHECK(ok, "4 channels on 4 bins");
    DSMIXBINVOLUMEPAIR p8[8];
    for (int i = 0; i < 8; i++) p8[i] = (DSMIXBINVOLUMEPAIR){ (DWORD)i, 0 };
    DSMIXBINS mb = { 8, p8 };
    CHECK(Voice_SetMixBins((void *)b, &mb) == DS_OK, "8 mixbins");
    static const int want[8] = { 0, 1, 0, 1, 2, 3, 2, 3 };
    ok = 1;
    for (int k = 0; k < 8; k++) if (voice_slot_channel(v, (DWORD)k) != want[k]) ok = 0;
    CHECK(ok, "4 channels on 8 bins: slots 0-3 carry channels 0/1, slots 4-7 channels 2/3");
    WAVEFORMATEX ws = pcm_format(2, 48000, 16);
    uint32_t bs = make_buffer(&ws, 0);
    struct ds_voice *vs = &find_buffer((void *)bs)->v;
    CHECK(Voice_SetMixBins((void *)bs, &mb) == DS_OK, "stereo on 8 mixbins");
    ok = 1;
    for (int k = 0; k < 8; k++) if (voice_slot_channel(vs, (DWORD)k) != (k & 1)) ok = 0;
    CHECK(ok, "stereo: left on even slots, right on odd ones");
    Obj_Release((void *)bs);
    Obj_Release((void *)b);
}

/* 3D distance and cone (CalcDistanceVolume / CalcConeVolume). */
static void test_3d_cone(void)
{
    uint32_t ds = 0;
    DirectSoundCreate(NULL, &ds, NULL);
    WAVEFORMATEX w = pcm_format(1, 48000, 16);
    uint32_t b = make_buffer(&w, DSBCAPS_CTRL3D);
    struct ds_voice *v = &find_buffer((void *)b)->v;
    /* The listener's distance factor scales velocities, not distances. */
    DS_SetDistanceFactor((void *)ds, 2.0f, 0);
    Voice_SetPosition((void *)b, 0, 0, 10, 0);
    voice_refresh(v);
    CHECK(v->l3d_vol == -2000, "10 m, rolloff 1: -2000 (got %d)", (int)v->l3d_vol);
    DS_SetDistanceFactor((void *)ds, 1.0f, 0);
    /* Source 1 m in front; cone 90 / 180 degrees (full apex angles), -2000 outside. */
    Voice_SetPosition((void *)b, 0, 0, 1, 0);
    Voice_SetConeAngles((void *)b, 90, 180, 0);
    Voice_SetConeOutsideVolume((void *)b, -2000, 0);
    Voice_SetConeOrientation((void *)b, 0, 0, -1, 0);       /* facing the listener */
    voice_refresh(v);
    CHECK(v->l3d_vol == 0, "facing the listener: %d", (int)v->l3d_vol);
    Voice_SetConeOrientation((void *)b, 0.7071f, 0, -0.7071f, 0);   /* 45 degrees off: inside the 90 cone */
    voice_refresh(v);
    CHECK(v->l3d_vol == 0, "45 degrees off axis is inside: %d", (int)v->l3d_vol);
    Voice_SetConeOrientation((void *)b, 0.9239f, 0, -0.3827f, 0);   /* 67.5 degrees: between the cones */
    voice_refresh(v);
    CHECK(v->l3d_vol < -500 && v->l3d_vol > -900, "67.5 degrees off axis: %d", (int)v->l3d_vol);
    Voice_SetConeOrientation((void *)b, 1, 0, 0, 0);        /* 90 degrees off: at the outside cone */
    voice_refresh(v);
    CHECK(v->l3d_vol == -2000, "90 degrees off axis: %d", (int)v->l3d_vol);
    Voice_SetConeOrientation((void *)b, 0, 0, 1, 0);        /* facing away */
    voice_refresh(v);
    CHECK(v->l3d_vol == -2000, "facing away: %d", (int)v->l3d_vol);
    Obj_Release((void *)b);
    Obj_Release((void *)ds);
}

/* Position notifications at offset 0 and across a loop wrap; SetCurrentPosition
   evaluates them too; StopEx and SetPlayRegion. */
static void test_buffer_control(void)
{
    WAVEFORMATEX w = pcm_format(1, 48000, 16);
    short pcm[1000];
    for (int i = 0; i < 1000; i++) pcm[i] = (short)i;
    uint32_t b = make_buffer(&w, DSBCAPS_CTRLPOSITIONNOTIFY);
    unity(b);
    Buf_SetBufferData((void *)b, pcm, sizeof(pcm));
    DSBPOSITIONNOTIFY n[2] = { { 0, (HANDLE)1 }, { 1000, (HANDLE)2 } };
    CHECK(Buf_SetNotificationPositions((void *)b, 2, n) == DS_OK, "notifications at 0 and 1000");
    memset(event_count, 0, sizeof(event_count));
    Buf_Play((void *)b, 0, 0, DSBPLAY_LOOPING);
    mix(64);
    CHECK(event_count[1] == 1 && event_count[2] == 0, "offset 0 fires once playback starts: %d %d",
          event_count[1], event_count[2]);
    mix(1000);
    CHECK(event_count[1] == 2 && event_count[2] == 1, "after a wrap: %d %d", event_count[1], event_count[2]);
    Buf_Stop((void *)b);

    /* A stopped buffer: SetCurrentPosition caches the cursor and signals
       the offsets it passed. */
    Buf_SetNotificationPositions((void *)b, 2, n);
    memset(event_count, 0, sizeof(event_count));
    CHECK(Buf_SetCurrentPosition((void *)b, 1200) == DS_OK, "SetCurrentPosition while stopped");
    CHECK(event_count[1] == 1 && event_count[2] == 1, "SetCurrentPosition evaluates notifications: %d %d",
          event_count[1], event_count[2]);
    DWORD play;
    Buf_GetCurrentPosition((void *)b, &play, NULL);
    CHECK(play == 1200, "cached cursor %u", play);
    Buf_Play((void *)b, 0, 0, 0);
    mix(4);
    CHECK(near(out[0], 0.5f * 600 / 32768.0f, 1e-6f), "Play resumes at the cached cursor");

    /* SetPlayRegion on a playing voice restarts it from the new region. */
    Buf_Play((void *)b, 0, 0, DSBPLAY_LOOPING);
    mix(30);
    CHECK(Buf_SetPlayRegion((void *)b, 400, 200) == DS_OK, "SetPlayRegion while playing");
    DWORD st;
    Buf_GetStatus((void *)b, &st);
    CHECK(st == (DSBSTATUS_PLAYING | DSBSTATUS_LOOPING), "still playing and looping: %#x", st);
    mix(150);
    CHECK(near(out[0], 0.5f * 200 / 32768.0f, 1e-6f) && near(out[2 * 100], 0.5f * 200 / 32768.0f, 1e-6f),
          "restarted at the region start and looping over it: %f %f", out[0], out[200]);

    /* StopEx without DSBSTOPEX_ENVELOPE is a plain Stop, even with RELEASEWAVEFORM. */
    CHECK(Buf_StopEx((void *)b, 0, 0, DSBSTOPEX_RELEASEWAVEFORM) == DS_OK, "StopEx(RELEASEWAVEFORM)");
    Buf_GetStatus((void *)b, &st);
    CHECK(st == 0, "StopEx(RELEASEWAVEFORM) stops: %#x", st);
    /* Deferred StopEx: the voice plays until the time comes. */
    Buf_Play((void *)b, 0, 0, DSBPLAY_LOOPING);
    int64_t now = 0;
    DS_GetTime(NULL, &now);
    int64_t at = now + 2000000;                      /* 200 ms = 9600 frames */
    CHECK(Buf_StopEx((void *)b, (uint32_t)at, (uint32_t)(at >> 32), 0) == DS_OK, "StopEx at a time");
    mix(4800);
    Buf_GetStatus((void *)b, &st);
    CHECK(st == (DSBSTATUS_PLAYING | DSBSTATUS_LOOPING), "playing before the stop time: %#x", st);
    mix(6000);
    Buf_GetStatus((void *)b, &st);
    CHECK(st == 0, "stopped at the stop time: %#x", st);
    Obj_Release((void *)b);
}

/* ---- streams ---- */

static struct { void *ctx, *pctx; DWORD status; } calls[64];
static int ncalls;

static void NTAPI stream_cb(void *ctx, void *pctx, DWORD status)
{
    if (ncalls < 64) { calls[ncalls].ctx = ctx; calls[ncalls].pctx = pctx; calls[ncalls].status = status; }
    ncalls++;
}

static void test_streams(void)
{
    WAVEFORMATEX w = pcm_format(1, 48000, 16);
    DSSTREAMDESC d = { 0, 3, &w, stream_cb, (void *)0x1234, NULL };
    uint32_t s = 0;
    CHECK(DirectSoundCreateStream(&d, &s) == DS_OK && s, "DirectSoundCreateStream");
    unity(s);
    /* The stream is a real vtable object: the XMediaObject slots work. */
    void **vt = *(void ***)s;
    HRESULT (NTAPI *getinfo)(void *, XMEDIAINFO *) = vt[2];
    XMEDIAINFO info;
    CHECK(getinfo((void *)s, &info) == DS_OK && info.dwFlags == 5 && info.dwInputSize == 2 &&
          info.dwMaxLookahead == 128, "GetInfo %u %u %u", info.dwFlags, info.dwInputSize, info.dwMaxLookahead);
    CHECK(Voice_SetVolume((void *)(s + 4), 0) == DS_OK && Voice_SetVolume((void *)(s + 0xC), 0) == DS_OK,
          "voice aliases p+4 / p+0xC");

    short *pk[4];
    DWORD done[4], status[4];
    for (int i = 0; i < 4; i++) {
        pk[i] = malloc(1000 * 2);
        for (int k = 0; k < 1000; k++) pk[i][k] = (short)(1000 * (i + 1));
        done[i] = 77; status[i] = 77;
    }
    DWORD st;
    Stream_GetStatus((void *)s, &st);
    CHECK(st == DSSTREAMSTATUS_READY, "empty stream: not playing before its first packet %#x", st);
    for (int i = 0; i < 3; i++) {
        XMEDIAPACKET xp = { pk[i], 2000, &done[i], &status[i], (void *)(uintptr_t)(0x100 + i), NULL };
        CHECK(Stream_Process((void *)s, &xp, NULL) == DS_OK, "Process %d", i);
        CHECK(status[i] == XMEDIAPACKET_STATUS_PENDING && done[i] == 0, "packet %d pending", i);
    }
    XMEDIAPACKET extra = { pk[3], 2000, &done[3], &status[3], (void *)0x103, NULL };
    CHECK(Stream_Process((void *)s, &extra, NULL) == DSERR_INVALIDCALL, "queue full");
    Stream_GetStatus((void *)s, &st);
    CHECK(st == DSSTREAMSTATUS_PLAYING, "full stream status %#x", st);
    XMEDIAPACKET odd = { pk[3], 3, &done[3], &status[3], NULL, NULL };
    CHECK(Stream_Process((void *)s, &odd, NULL) == DSERR_INVALIDPARAM, "unaligned packet rejected");

    ncalls = 0;
    mix(1500);
    /* Gapless, in order. */
    CHECK(near(out[2 * 999], 1000 / 65536.0f, 1e-5f) && near(out[2 * 1000], 2000 / 65536.0f, 1e-5f),
          "packets play back to back: %f %f", out[2 * 999], out[2 * 1000]);
    CHECK(ncalls == 0 && status[0] == XMEDIAPACKET_STATUS_PENDING, "completions wait for DoWork");
    DirectSoundDoWork();
    CHECK(ncalls == 1 && calls[0].ctx == (void *)0x1234 && calls[0].pctx == (void *)0x100 && calls[0].status == 0,
          "first completion: %d calls", ncalls);
    CHECK(status[0] == 0 && done[0] == 2000 && status[1] == XMEDIAPACKET_STATUS_PENDING, "status and size");
    Stream_GetStatus((void *)s, &st);
    CHECK(st & DSSTREAMSTATUS_READY, "a slot is free again %#x", st);
    mix(1600);
    /* All three consumed: the stream starves, completes in order. */
    DirectSoundDoWork();
    CHECK(ncalls == 3 && calls[1].pctx == (void *)0x101 && calls[2].pctx == (void *)0x102, "completion order");
    Stream_GetStatus((void *)s, &st);
    CHECK(st == (DSSTREAMSTATUS_READY | DSSTREAMSTATUS_PAUSED | DSSTREAMSTATUS_STARVED), "starved %#x", st);
    CHECK(out[2 * 1599] == 0, "silence when starved");

    /* Pause keeps packets; Flush completes them with FLUSHED synchronously. */
    XMEDIAPACKET xp = { pk[3], 2000, &done[3], &status[3], (void *)0x103, NULL };
    Stream_Process((void *)s, &xp, NULL);
    Stream_Pause((void *)s, 1);
    mix(512);
    CHECK(out[0] == 0 && status[3] == XMEDIAPACKET_STATUS_PENDING, "paused");
    Stream_GetStatus((void *)s, &st);
    CHECK(st == (DSSTREAMSTATUS_READY | DSSTREAMSTATUS_PAUSED), "paused status %#x", st);
    ncalls = 0;
    HRESULT (NTAPI *flush)(void *) = vt[6];
    CHECK(flush((void *)s) == DS_OK && ncalls == 1 && calls[0].status == XMEDIAPACKET_STATUS_FLUSHED &&
          status[3] == XMEDIAPACKET_STATUS_FLUSHED, "Flush");

    /* Discontinuity: the stream stops (not starved) when the queue drains. */
    Stream_Pause((void *)s, 0);
    Stream_Process((void *)s, &xp, NULL);
    HRESULT (NTAPI *discontinuity)(void *) = vt[5];
    discontinuity((void *)s);
    mix(1100);
    DirectSoundDoWork();
    Stream_GetStatus((void *)s, &st);
    CHECK(st == DSSTREAMSTATUS_READY && status[3] == 0, "discontinuity ends the stream: %#x", st);

    /* Release flushes pending packets through the callback, synchronously. */
    Stream_Process((void *)s, &xp, NULL);
    ncalls = 0;
    ULONG (NTAPI *release)(void *) = vt[1];
    CHECK(release((void *)s) == 0 && ncalls == 1 && calls[0].status == XMEDIAPACKET_STATUS_FLUSHED,
          "Release flushes");

    /* Without a callback the packet's event is signalled. */
    DSSTREAMDESC d2 = { 0, 2, &w, NULL, NULL, NULL };
    uint32_t s2 = 0;
    DirectSoundCreateStream(&d2, &s2);
    memset(event_count, 0, sizeof(event_count));
    XMEDIAPACKET ep = { pk[0], 200, &done[0], &status[0], (HANDLE)5, NULL };
    Stream_Process((void *)s2, &ep, NULL);
    mix(150);
    DirectSoundDoWork();
    CHECK(event_count[5] == 1 && status[0] == 0 && done[0] == 200, "completion event");
    Obj_Release((void *)s2);
    for (int i = 0; i < 4; i++) free(pk[i]);
}

/* Release of an ACCURATENOTIFY stream: its completed packets are reported
   first, in order, then the flushed ones; another stream's queued
   completions stay where they are. */
static void test_stream_release_order(void)
{
    WAVEFORMATEX w = pcm_format(1, 48000, 16);
    DSSTREAMDESC da = { DSSTREAMCAPS_ACCURATENOTIFY, 3, &w, stream_cb, (void *)0xA, NULL };
    DSSTREAMDESC db = { DSSTREAMCAPS_ACCURATENOTIFY, 3, &w, stream_cb, (void *)0xB, NULL };
    uint32_t sa = 0, sb = 0;
    CHECK(DirectSoundCreateStream(&da, &sa) == DS_OK && DirectSoundCreateStream(&db, &sb) == DS_OK,
          "two ACCURATENOTIFY streams");
    static short pk[3][200];
    DWORD done[3], status[3];
    XMEDIAPACKET a1 = { pk[0], 200, &done[0], &status[0], (void *)0xA1, NULL };
    XMEDIAPACKET a2 = { pk[1], 400, &done[1], &status[1], (void *)0xA2, NULL };
    XMEDIAPACKET b1 = { pk[2], 200, &done[2], &status[2], (void *)0xB1, NULL };
    Stream_Process((void *)sa, &a1, NULL);
    Stream_Process((void *)sa, &a2, NULL);
    Stream_Process((void *)sb, &b1, NULL);
    ncalls = 0;
    mix(150);                                        /* a1 and b1 complete, a2 is half played */
    CHECK(ncalls == 0, "completions are queued, not delivered by the mixer");
    ULONG (NTAPI *release)(void *) = (*(void ***)sa)[1];
    CHECK(release((void *)sa) == 0, "Release");
    CHECK(ncalls == 2 && calls[0].pctx == (void *)0xA1 && calls[0].status == XMEDIAPACKET_STATUS_SUCCESS &&
          calls[1].pctx == (void *)0xA2 && calls[1].status == XMEDIAPACKET_STATUS_FLUSHED,
          "own completions first, then the flushed packet (%d calls)", ncalls);
    CHECK(done[1] == 400, "a flushed packet reports dwMaxSize as completed: %u", done[1]);
    DirectSoundDoWork();
    CHECK(ncalls == 3 && calls[2].ctx == (void *)0xB && calls[2].pctx == (void *)0xB1, "the other stream's completion");
    Obj_Release((void *)sb);
}

static void test_wave_xmo(void)
{
    /* A small RIFF file: 16-bit stereo, 10 frames, with a wsmp loop of frames 2..6. */
    char path[] = "/tmp/dsound_test_XXXXXX";
    int fd = mkstemp(path);
    uint8_t f[128]; size_t n = 0;
    #define PUT(p, len) do { memcpy(f + n, p, len); n += len; } while (0)
    #define U32(x) do { uint32_t v_ = (x); PUT(&v_, 4); } while (0)
    WAVEFORMATEX w = pcm_format(2, 22050, 16);
    PUT("RIFF", 4); U32(0); PUT("WAVE", 4);
    PUT("fmt ", 4); U32(16); PUT(&w, 16);
    PUT("wsmp", 4); U32(36); U32(20); U32(0); U32(0); U32(0); U32(1); U32(16); U32(0); U32(2); U32(4);
    PUT("data", 4); U32(40);
    for (int i = 0; i < 20; i++) { int16_t v = (int16_t)i; PUT(&v, 2); }
    uint32_t riff = (uint32_t)n - 8; memcpy(f + 4, &riff, 4);
    CHECK(write(fd, f, n) == (ssize_t)n, "write wav");
    close(fd);
    const WAVEFORMATEX *wf = NULL;
    void *x = NULL;
    CHECK(XWaveFileCreateMediaObject(path, &wf, &x) == DS_OK && wf->nChannels == 2 && wf->nSamplesPerSec == 22050,
          "XWaveFileCreateMediaObject");
    DWORD len = 0, start = 0, llen = 0;
    CHECK(Wave_GetLength(x, &len) == DS_OK && len == 40, "GetLength %u", len);
    CHECK(Wave_GetLoopRegion(x, &start, &llen) == DS_OK && start == 8 && llen == 16, "loop region %u %u", start, llen);
    int16_t buf[20]; DWORD got = 0, status = 1;
    XMEDIAPACKET p = { buf, 18, &got, &status, NULL, NULL };    /* rounds down to 4 frames */
    CHECK(Wave_Process(x, NULL, &p) == DS_OK && got == 16 && status == 0 && buf[7] == 7, "Process reads %u", got);
    p.dwMaxSize = 40;
    Wave_Process(x, NULL, &p);
    CHECK(got == 24 && buf[0] == 8, "Process reads the rest: %u", got);
    Wave_Process(x, NULL, &p);
    CHECK(got == 0 && status == 0, "EOF completes with 0 bytes");
    DWORD pos = 0;
    Wave_Seek(x, 4, FILE_BEGIN, &pos);
    CHECK(pos == 4, "Seek");
    Wave_Release(x);
    unlink(path);
}

static void test_effects(void)
{
    uint32_t ds = 0;
    DirectSoundCreate(NULL, &ds, NULL);
    /* image: 2048 bytes, command block {0, code 2, 0, state 1, 0, 0}, code, state, descriptor */
    size_t sz = 2048 + 24 + 8 + 4 + 8 + 32 * 2 + 16;
    uint8_t *img = calloc(1, sz);
    uint32_t cmd[6] = { 0, 2, 0, 1, 0, 0 };
    memcpy(img + 2048, cmd, 24);
    uint32_t *desc = (uint32_t *)(img + 2048 + 24 + 12);
    desc[0] = 2; desc[1] = 0;
    desc[2 + 3] = 4;            /* effect 0: dwStateSize 4 DWORDs */
    desc[2 + 8 + 3] = 2;        /* effect 1: 2 DWORDs */
    DSEFFECTIMAGEDESC *pd = NULL;
    CHECK(DS_DownloadEffectsImage((void *)ds, img, (DWORD)sz, NULL, &pd) == DS_OK && pd && pd->dwEffectCount == 2,
          "DownloadEffectsImage");
    DWORD v = 0x123456, r = 0;
    CHECK(DS_SetEffectData((void *)ds, 0, 12, &v, 4, 0) == DS_OK, "SetEffectData");
    CHECK(DS_GetEffectData((void *)ds, 0, 12, &r, 4) == DS_OK && r == v, "GetEffectData reads back");
    CHECK(DS_SetEffectData((void *)ds, 1, 8, &v, 4, 0) == DSERR_INVALIDPARAM, "out of the state range");
    CHECK(DS_SetEffectData((void *)ds, 2, 0, &v, 4, 0) == DSERR_INVALIDPARAM, "bad effect index");
    CHECK(DS_CommitEffectData((void *)ds) == DS_OK, "CommitEffectData");
    Obj_Release((void *)ds);
    free(img);
}

static void test_sdl_device(void)
{
    /* The real device: the dummy driver (or a silent clock) advances time. */
    g_ds_no_audio_thread = 0;
    g.audio_mode = 0;
    quiet = 0;
    uint32_t ds = 0;
    DirectSoundCreate(NULL, &ds, NULL);
    CHECK(g.audio_mode == 1 || g.audio_mode == 2, "audio started (mode %d)", g.audio_mode);
    DWORD t0 = DirectSoundGetSampleTime();
    usleep(200000);
    DWORD t1 = DirectSoundGetSampleTime();
    CHECK(t1 > t0 + 2000, "sample clock runs: %u -> %u", t0, t1);
    int64_t rt = 0;
    DS_GetTime((void *)ds, &rt);
    CHECK(rt > 0, "GetTime");
    Obj_Release((void *)ds);
}

int main(int argc, char **argv)
{
    g_ds_no_audio_thread = 1;
    quiet = !getenv("DSOUND_TEST_VERBOSE");
    test_formats();
    test_object_aliases();
    test_playback();
    test_legacy_mixbin_headroom();
    test_looping_and_regions();
    test_pitch();
    test_adpcm(argc > 1 ? argv[1] : NULL);
    test_notifications();
    test_lock();
    test_3d();
    test_submix();
    test_submix_routing();
    test_channel_slots();
    test_3d_cone();
    test_buffer_control();
    test_streams();
    test_stream_release_order();
    test_wave_xmo();
    test_effects();
    test_sdl_device();
    printf("dsound_test: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
