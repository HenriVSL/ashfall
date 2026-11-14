/* e_audio.cpp - DirectSound voices with cached parameters committed once per frame (DESIGN 2.5, 8.6). Windows.
   Owner: T08. See e_audio.h for the semantics. Patterns from Retro Strike's snd.cpp (same VM): chunk-walking WAV
   parser, BUFFERLOST -> Restore + refill, busy state from timeGetTime instead of GetStatus. */
#define WIN32_LEAN_AND_MEAN
#define _ENABLE_AUTODEPEND          /* sdk/README.md preamble: dsound.h pulls in d3dtypes.h, which sdk\ provides */
#define DIRECTDRAW_VERSION 0x0600
#define DIRECT3D_VERSION   0x0600
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>

#include "e_audio.h"
#include "e_log.h"
#include "e_file.h"
#include "e_plat.h"
#include "e_cfg.h"

typedef HRESULT (WINAPI *DSCreateFn)(LPGUID, LPDIRECTSOUND *, LPUNKNOWN);

typedef struct Sound {
    char name[48];
    char path[E_PATH];
    u8 *file;                       /* whole file (file_load) */
    const u8 *pcm; int bytes;
    WAVEFORMATEX wf;
    int frames;
} Sound;

typedef struct Voice {
    LPDIRECTSOUNDBUFFER buf;
    int snd;                        /* sound index or -1 */
    int gen;                        /* handle generation (stale handles are ignored) */
    int hw, maxFreq;
    int busy, loop;
    DWORD endMs;
    u32 order;
    float vol, pan, pitch;          /* targets (au_set) */
    int curVol, curPan; DWORD curFreq; float curPanF;
} Voice;

static HMODULE        s_dll;
static LPDIRECTSOUND  s_ds;
static LPDIRECTSOUNDBUFFER s_primary;
static int s_repanAfterFreq = 1;                  /* -norepan turns it off */
static int            s_ok;
static int            s_globalFocus;
static Sound          s_snd[AU_MAX_SOUNDS];
static int            s_numSnd;
static Voice          s_voice[AU_MAX_VOICES];
static float          s_master = 1.0f;
static int            s_masterDirty;
static u32            s_order;
static AuStats        s_st;
static int            s_errLogged;

/* ---------------------------------------------------------------- helpers */
static const char *hr_str(HRESULT hr)
{
    static char buf[16];
    switch (hr) {
    case DS_OK:                 return "DS_OK";
    case DSERR_ALLOCATED:       return "ALLOCATED";
    case DSERR_BADFORMAT:       return "BADFORMAT";
    case DSERR_BUFFERLOST:      return "BUFFERLOST";
    case DSERR_CONTROLUNAVAIL:  return "CONTROLUNAVAIL";
    case DSERR_INVALIDCALL:     return "INVALIDCALL";
    case DSERR_INVALIDPARAM:    return "INVALIDPARAM";
    case DSERR_NODRIVER:        return "NODRIVER";
    case DSERR_OUTOFMEMORY:     return "OUTOFMEMORY";
    case DSERR_PRIOLEVELNEEDED: return "PRIOLEVELNEEDED";
    case DSERR_UNSUPPORTED:     return "UNSUPPORTED";
    }
    e_snprintf(buf, (int)sizeof(buf), "0x%08lX", (unsigned long)hr);
    return buf;
}

static void err(const char *what, HRESULT hr)
{
    s_st.errors++;
    if (s_errLogged++ < 20) log_printf(LOG_WARN, "warn: snd %s failed hr=%s", what, hr_str(hr));
}

static int vol_to_ds(float v)
{
    double db;
    if (v <= 0.0001f) return DSBVOLUME_MIN;
    if (v >= 1.0f) return 0;
    db = 2000.0 * log10((double)v);
    if (db < -10000.0) db = -10000.0;
    return (int)(db - 0.5);
}

static int pan_to_ds(float p)
{
    double a;
    if (p > 0.999f) return DSBPAN_RIGHT;
    if (p < -0.999f) return DSBPAN_LEFT;
    a = 2000.0 * log10(1.0 - fabs((double)p));      /* <= 0: attenuation of the far channel */
    if (a < -10000.0) a = -10000.0;
    return p >= 0.0f ? (int)(-a + 0.5) : (int)(a - 0.5);
}

static int rd16(const u8 *p) { return p[0] | (p[1] << 8); }
static int rd32(const u8 *p) { return (int)(p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24)); }

static int wav_parse(Sound *s, int size)
{
    const u8 *p = s->file, *end = s->file + size;
    int len, haveFmt = 0;
    if (size < 12 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) return 0;
    p += 12;
    s->pcm = NULL;
    while (p + 8 <= end) {
        len = rd32(p + 4);
        if (len < 0 || p + 8 + len > end) len = (int)(end - p - 8);
        if (!memcmp(p, "fmt ", 4) && len >= 16) {
            memset(&s->wf, 0, sizeof(s->wf));
            s->wf.wFormatTag = (WORD)rd16(p + 8);
            s->wf.nChannels = (WORD)rd16(p + 10);
            s->wf.nSamplesPerSec = (DWORD)rd32(p + 12);
            s->wf.wBitsPerSample = (WORD)rd16(p + 22);
            s->wf.nBlockAlign = (WORD)(s->wf.nChannels * s->wf.wBitsPerSample / 8);
            s->wf.nAvgBytesPerSec = s->wf.nSamplesPerSec * s->wf.nBlockAlign;
            haveFmt = 1;
        } else if (!memcmp(p, "data", 4)) {
            s->pcm = p + 8;
            s->bytes = len;
        }
        p += 8 + len + (len & 1);
    }
    if (!haveFmt || !s->pcm || s->wf.wFormatTag != WAVE_FORMAT_PCM || s->wf.nChannels < 1 || s->wf.nChannels > 2 ||
        (s->wf.wBitsPerSample != 8 && s->wf.wBitsPerSample != 16) || s->wf.nSamplesPerSec < 4000)
        return 0;
    s->bytes -= s->bytes % s->wf.nBlockAlign;
    s->frames = s->bytes / s->wf.nBlockAlign;
    return s->bytes > 0;
}

static int fill(LPDIRECTSOUNDBUFFER b, const Sound *s)
{
    void *p1, *p2;
    DWORD n1, n2;
    HRESULT hr = b->Lock(0, (DWORD)s->bytes, &p1, &n1, &p2, &n2, 0);
    if (hr == DSERR_BUFFERLOST) { b->Restore(); hr = b->Lock(0, (DWORD)s->bytes, &p1, &n1, &p2, &n2, 0); }
    if (hr != DS_OK) { err("Lock", hr); return 0; }
    memcpy(p1, s->pcm, n1);
    if (p2 && n2) memcpy(p2, s->pcm + n1, n2);
    b->Unlock(p1, n1, p2, n2);
    return 1;
}

static DWORD base_flags(void)
{
    return DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN | DSBCAPS_CTRLFREQUENCY | DSBCAPS_STATIC |
           DSBCAPS_GETCURRENTPOSITION2 | (s_globalFocus ? DSBCAPS_GLOBALFOCUS : 0);
}

/* a buffer for the format of s (bytes of silence when s->pcm is NULL); *hw = hardware-mixed */
static LPDIRECTSOUNDBUFFER make_buffer(const WAVEFORMATEX *wf, int bytes, DWORD extra, int *hw)
{
    DSBUFFERDESC d;
    DSBCAPS bc;
    LPDIRECTSOUNDBUFFER b = NULL;
    WAVEFORMATEX f = *wf;
    HRESULT hr;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwFlags = base_flags() | extra;
    d.dwBufferBytes = (DWORD)bytes;
    d.lpwfxFormat = &f;
    hr = s_ds->CreateSoundBuffer(&d, &b, NULL);
    if (hr != DS_OK && !(extra & DSBCAPS_LOCSOFTWARE)) {   /* hardware out of voices/memory: software */
        d.dwFlags = base_flags() | extra | DSBCAPS_LOCSOFTWARE;
        hr = s_ds->CreateSoundBuffer(&d, &b, NULL);
    }
    if (hr != DS_OK) { err("CreateSoundBuffer", hr); return NULL; }
    *hw = 0;
    memset(&bc, 0, sizeof(bc));
    bc.dwSize = sizeof(bc);
    if (b->GetCaps(&bc) == DS_OK) *hw = (bc.dwFlags & DSBCAPS_LOCHARDWARE) ? 1 : 0;
    return b;
}

/* ---------------------------------------------------------------- call timing (DESIGN 8.6) */
static int us_avg(s64 sum, int n) { return n > 0 ? (int)(sum / n) : 0; }

static void measure_calls(void)
{
    WAVEFORMATEX wf;
    LPDIRECTSOUNDBUFFER b;
    int hw, i, n = 32;
    s64 t, sSet = 0, sFreq = 0, sPan = 0, sStat = 0, sPos = 0, sPlay = 0, sStop = 0, sCreate;
    DWORD st, pc, wc;
    void *p1, *p2;
    DWORD n1, n2;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 1;
    wf.nSamplesPerSec = 22050;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 2;
    wf.nAvgBytesPerSec = 44100;
    t = plat_now_us();
    b = make_buffer(&wf, 4410, 0, &hw);
    sCreate = plat_now_us() - t;
    if (!b) return;
    if (b->Lock(0, 4410, &p1, &n1, &p2, &n2, 0) == DS_OK) {
        memset(p1, 0, n1);
        if (p2 && n2) memset(p2, 0, n2);
        b->Unlock(p1, n1, p2, n2);
    }
    b->SetVolume(DSBVOLUME_MIN);
    b->Play(0, 0, DSBPLAY_LOOPING);
    for (i = 0; i < n; i++) {
        t = plat_now_us(); b->SetVolume(-9000 - (i & 1) * 100);  sSet += plat_now_us() - t;
        t = plat_now_us(); b->SetFrequency(20000 + (i & 7) * 500); sFreq += plat_now_us() - t;
        t = plat_now_us(); b->SetPan((i & 1) ? 300 : -300);       sPan += plat_now_us() - t;
        t = plat_now_us(); b->GetStatus(&st);                      sStat += plat_now_us() - t;
        t = plat_now_us(); b->GetCurrentPosition(&pc, &wc);        sPos += plat_now_us() - t;
        t = plat_now_us(); b->Stop();                              sStop += plat_now_us() - t;
        t = plat_now_us(); b->Play(0, 0, DSBPLAY_LOOPING);         sPlay += plat_now_us() - t;
    }
    b->Stop();
    b->Release();
    s_st.callUs.set = us_avg(sSet, n);
    s_st.callUs.freq = us_avg(sFreq, n);
    s_st.callUs.pan = us_avg(sPan, n);
    s_st.callUs.status = us_avg(sStat, n);
    s_st.callUs.pos = us_avg(sPos, n);
    s_st.callUs.play = us_avg(sPlay, n);
    s_st.callUs.stop = us_avg(sStop, n);
    s_st.callUs.create = (int)sCreate;
    log_printf(LOG_INFO, "snd: call_us set=%d freq=%d pan=%d status=%d pos=%d play=%d stop=%d create=%d hw=%d n=%d",
               s_st.callUs.set, s_st.callUs.freq, s_st.callUs.pan, s_st.callUs.status, s_st.callUs.pos,
               s_st.callUs.play, s_st.callUs.stop, s_st.callUs.create, hw, n);
}

/* ---------------------------------------------------------------- init / shutdown */
int au_init(void *hwnd, int rate)
{
    DSCreateFn create;
    HRESULT hr;
    DSCAPS caps;
    DSBUFFERDESC d;
    WAVEFORMATEX wf;
    int i, coop = DSSCL_PRIORITY;
    au_shutdown();
    memset(&s_st, 0, sizeof(s_st));
    s_errLogged = 0;
    for (i = 0; i < AU_MAX_VOICES; i++) { memset(&s_voice[i], 0, sizeof(s_voice[i])); s_voice[i].snd = -1; }
    if (!hwnd) { log_printf(LOG_INFO, "snd: no window, silent mode"); return 0; }
    if (rate < 8000 || rate > 48000) rate = 22050;
    s_dll = LoadLibraryA("dsound.dll");
    create = s_dll ? (DSCreateFn)GetProcAddress(s_dll, "DirectSoundCreate") : NULL;
    if (!create) { log_printf(LOG_INFO, "snd: dsound.dll not available, silent mode"); au_shutdown(); return 0; }
    hr = create(NULL, &s_ds, NULL);
    if (hr != DS_OK || !s_ds) {
        log_printf(LOG_INFO, "snd: DirectSoundCreate failed hr=%s, silent mode", hr_str(hr));
        s_ds = NULL;
        au_shutdown();
        return 0;
    }
    s_repanAfterFreq = !arg_has("-norepan");
    hr = s_ds->SetCooperativeLevel((HWND)hwnd, DSSCL_PRIORITY);
    if (hr != DS_OK) {
        coop = DSSCL_NORMAL;
        hr = s_ds->SetCooperativeLevel((HWND)hwnd, DSSCL_NORMAL);
    }
    if (hr != DS_OK) {
        log_printf(LOG_INFO, "snd: SetCooperativeLevel failed hr=%s, silent mode", hr_str(hr));
        au_shutdown();
        return 0;
    }
    if (coop == DSSCL_PRIORITY) {
        memset(&d, 0, sizeof(d));
        d.dwSize = sizeof(d);
        d.dwFlags = DSBCAPS_PRIMARYBUFFER;
        hr = s_ds->CreateSoundBuffer(&d, &s_primary, NULL);
        if (hr == DS_OK) {
            memset(&wf, 0, sizeof(wf));
            wf.wFormatTag = WAVE_FORMAT_PCM;
            wf.nChannels = 2;
            wf.nSamplesPerSec = (DWORD)rate;
            wf.wBitsPerSample = 16;
            wf.nBlockAlign = 4;
            wf.nAvgBytesPerSec = (DWORD)rate * 4;
            hr = s_primary->SetFormat(&wf);
            if (hr != DS_OK) log_printf(LOG_WARN, "warn: snd primary SetFormat %d Hz failed hr=%s", rate, hr_str(hr));
            memset(&wf, 0, sizeof(wf));
            s_primary->GetFormat(&wf, sizeof(wf), NULL);
            log_printf(LOG_INFO, "snd: primary rate=%lu bits=%d ch=%d", (unsigned long)wf.nSamplesPerSec,
                       wf.wBitsPerSample, wf.nChannels);
        } else {
            s_primary = NULL;
            log_printf(LOG_WARN, "warn: snd primary buffer hr=%s", hr_str(hr));
        }
    }
    memset(&caps, 0, sizeof(caps));
    caps.dwSize = sizeof(caps);
    if (s_ds->GetCaps(&caps) == DS_OK) {
        s_st.hwMaxFreq = (int)caps.dwMaxSecondarySampleRate;
        log_printf(LOG_INFO, "snd: caps flags=0x%lX emul=%d hw_buffers=%lu free=%lu hw_static=%lu sec_rate=%lu..%lu "
                   "hw_mem=%lu", (unsigned long)caps.dwFlags, (caps.dwFlags & DSCAPS_EMULDRIVER) ? 1 : 0,
                   (unsigned long)caps.dwMaxHwMixingAllBuffers, (unsigned long)caps.dwFreeHwMixingAllBuffers,
                   (unsigned long)caps.dwMaxHwMixingStaticBuffers, (unsigned long)caps.dwMinSecondarySampleRate,
                   (unsigned long)caps.dwMaxSecondarySampleRate, (unsigned long)caps.dwTotalHwMemBytes);
    }
    s_ok = 1;
    s_master = 1.0f;
    measure_calls();
    log_printf(LOG_INFO, "snd: init ok coop=%s voices=%d global_focus=%d", coop == DSSCL_PRIORITY ? "priority" : "normal",
               AU_MAX_VOICES, s_globalFocus);
    return 1;
}

static void voice_release(Voice *v)
{
    if (v->buf) { v->buf->Stop(); v->buf->Release(); }
    v->buf = NULL;
    v->snd = -1;
    v->busy = 0;
}

void au_shutdown(void)
{
    int i;
    for (i = 0; i < AU_MAX_VOICES; i++) voice_release(&s_voice[i]);
    for (i = 0; i < s_numSnd; i++) if (s_snd[i].file) free(s_snd[i].file);
    memset(s_snd, 0, sizeof(s_snd));
    s_numSnd = 0;
    if (s_primary) s_primary->Release();
    s_primary = NULL;
    if (s_ds) s_ds->Release();
    s_ds = NULL;
    if (s_dll) FreeLibrary(s_dll);
    s_dll = NULL;
    s_ok = 0;
}

int  au_ok(void) { return s_ok; }
void au_set_global_focus(int on) { s_globalFocus = on ? 1 : 0; }

/* ---------------------------------------------------------------- sounds */
ASnd au_load(const char *wavPath)
{
    Sound *s;
    int i, size = 0;
    const char *base;
    if (!s_ok || !wavPath) return 0;
    for (i = 0; i < s_numSnd; i++) if (!e_stricmp(s_snd[i].path, wavPath)) return i + 1;
    if (s_numSnd >= AU_MAX_SOUNDS) { log_printf(LOG_WARN, "warn: snd too many sounds path=%s", wavPath); return 0; }
    s = &s_snd[s_numSnd];
    memset(s, 0, sizeof(*s));
    s->file = (u8 *)file_load(wavPath, &size);
    if (!s->file) { log_printf(LOG_WARN, "warn: snd cannot open path=%s", wavPath); return 0; }
    if (!wav_parse(s, size)) {
        log_printf(LOG_WARN, "warn: snd unsupported wav path=%s", wavPath);
        free(s->file);
        memset(s, 0, sizeof(*s));
        return 0;
    }
    e_strlcpy(s->path, wavPath, (int)sizeof(s->path));
    base = wavPath + strlen(wavPath);
    while (base > wavPath && base[-1] != '\\' && base[-1] != '/') base--;
    e_strlcpy(s->name, base, (int)sizeof(s->name));
    log_printf(LOG_DEBUG, "snd: load name=%s rate=%lu bits=%d ch=%d frames=%d", s->name,
               (unsigned long)s->wf.nSamplesPerSec, s->wf.wBitsPerSample, s->wf.nChannels, s->frames);
    s_numSnd++;
    return s_numSnd;
}

int au_snd_rate(ASnd s) { return (s >= 1 && s <= s_numSnd) ? (int)s_snd[s - 1].wf.nSamplesPerSec : 0; }
int au_snd_samples(ASnd s) { return (s >= 1 && s <= s_numSnd) ? s_snd[s - 1].frames : 0; }

/* ---------------------------------------------------------------- voices */
static Voice *voice_of(AVoice h)
{
    int i = (h & 0xFF) - 1;
    if (!s_ok || i < 0 || i >= AU_MAX_VOICES) return NULL;
    if (s_voice[i].gen != (h >> 8) || !s_voice[i].buf) return NULL;
    return &s_voice[i];
}

static void voice_refresh_busy(Voice *v, DWORD now)
{
    if (v->busy && !v->loop && (int)(now - v->endMs) >= 0) v->busy = 0;
}

static DWORD voice_freq(const Voice *v, float pitch)
{
    double f = (double)s_snd[v->snd].wf.nSamplesPerSec * (double)pitch;
    if (f < DSBFREQUENCY_MIN) f = DSBFREQUENCY_MIN;
    if (f > (double)v->maxFreq) f = (double)v->maxFreq;
    return (DWORD)(f + 0.5);
}

static int voice_bind(Voice *v, int snd)
{
    int hw = 0;
    if (v->buf && v->snd == snd) return 1;
    voice_release(v);
    v->buf = make_buffer(&s_snd[snd].wf, s_snd[snd].bytes, 0, &hw);
    if (!v->buf) return 0;
    if (!fill(v->buf, &s_snd[snd])) { voice_release(v); return 0; }
    v->snd = snd;
    v->hw = hw;
    v->maxFreq = DSBFREQUENCY_MAX;
    if (hw && s_st.hwMaxFreq > 0 && s_st.hwMaxFreq < DSBFREQUENCY_MAX) v->maxFreq = s_st.hwMaxFreq;
    return 1;
}

static HRESULT voice_start(Voice *v)
{
    HRESULT hr;
    v->buf->SetCurrentPosition(0);
    v->buf->SetVolume(v->curVol);
    v->buf->SetPan(v->curPan);
    v->buf->SetFrequency(v->curFreq);
    hr = v->buf->Play(0, 0, v->loop ? DSBPLAY_LOOPING : 0);
    if (hr == DSERR_BUFFERLOST) {
        v->buf->Restore();
        fill(v->buf, &s_snd[v->snd]);
        hr = v->buf->Play(0, 0, v->loop ? DSBPLAY_LOOPING : 0);
    }
    return hr;
}

AVoice au_play(ASnd s, float vol, float pan, float pitch, int loop)
{
    int i, best = -1, fresh = -1, other = -1, oldest = -1, snd = s - 1;
    DWORD now;
    Voice *v;
    HRESULT hr;
    if (!s_ok || snd < 0 || snd >= s_numSnd) return 0;
    now = timeGetTime();
    for (i = 0; i < AU_MAX_VOICES; i++) {
        Voice *t = &s_voice[i];
        voice_refresh_busy(t, now);
        if (t->busy) {
            if (!t->loop && (oldest < 0 || t->order < s_voice[oldest].order)) oldest = i;
            continue;
        }
        if (t->buf && t->snd == snd) { if (best < 0) best = i; }
        else if (!t->buf) { if (fresh < 0) fresh = i; }
        else if (other < 0 || t->order < s_voice[other].order) other = i;
    }
    if (best < 0) best = fresh;
    if (best < 0) best = other;
    if (best < 0 && oldest >= 0) { best = oldest; s_st.steals++; }
    if (best < 0) { s_st.fails++; return 0; }
    v = &s_voice[best];
    if (v->busy) v->buf->Stop();
    v->busy = 0;
    if (!voice_bind(v, snd)) { s_st.fails++; return 0; }
    v->loop = loop ? 1 : 0;
    v->vol = vol; v->pan = pan; v->pitch = pitch;
    v->curVol = vol_to_ds(vol * s_master);
    v->curPan = pan_to_ds(pan);
    v->curPanF = pan;
    v->curFreq = voice_freq(v, pitch);
    hr = voice_start(v);
    if (hr != DS_OK) { err("Play", hr); s_st.fails++; return 0; }
    v->busy = 1;
    v->endMs = now + (DWORD)((double)s_snd[snd].frames * 1000.0 / (double)v->curFreq) + 20;
    v->order = ++s_order;
    v->gen = (v->gen + 1) & 0xFFFFF;
    if (!v->gen) v->gen = 1;
    s_st.plays++;
    return (v->gen << 8) | (best + 1);
}

void au_set(AVoice h, float vol, float pan, float pitch)
{
    Voice *v = voice_of(h);
    if (!v || !v->busy) return;
    v->vol = vol; v->pan = pan; v->pitch = pitch;
}

void au_stop(AVoice h)
{
    Voice *v = voice_of(h);
    if (!v || !v->busy) return;
    v->buf->Stop();
    v->busy = 0;
}

int au_playing(AVoice h)
{
    Voice *v = voice_of(h);
    if (!v) return 0;
    voice_refresh_busy(v, timeGetTime());
    return v->busy;
}

void au_master(float vol)
{
    if (!s_ok) return;
    s_master = E_CLAMP(vol, 0.0f, 1.0f);
    s_masterDirty = 1;
}

void au_update(void)
{
    int i, calls = 0, dv, busy = 0, hw = 0, sw = 0;
    DWORD now, f, df;
    s64 t0, us;
    if (!s_ok) return;
    t0 = plat_now_us();
    now = timeGetTime();
    for (i = 0; i < AU_MAX_VOICES; i++) {
        Voice *v = &s_voice[i];
        voice_refresh_busy(v, now);
        if (!v->busy) continue;
        busy++;
        if (v->hw) hw++; else sw++;
        dv = vol_to_ds(v->vol * s_master);
        if (dv != v->curVol && (dv - v->curVol >= 50 || v->curVol - dv >= 50 || dv == DSBVOLUME_MIN || dv == 0 ||
                                v->curVol == DSBVOLUME_MIN)) {
            v->buf->SetVolume(dv);
            v->curVol = dv;
            calls++;
        }
        if (v->curVol == DSBVOLUME_MIN) continue;         /* silent: pan/pitch can wait until it is audible */
        if ((v->pan - v->curPanF >= 0.02f || v->curPanF - v->pan >= 0.02f) ||
            (v->pan == 0.0f && v->curPanF != 0.0f)) {
            v->buf->SetPan(pan_to_ds(v->pan));
            v->curPanF = v->pan;
            calls++;
        }
        f = voice_freq(v, v->pitch);
        df = f > v->curFreq ? f - v->curFreq : v->curFreq - f;
        if (df > 0 && (df * 200 >= v->curFreq || f == (DWORD)v->maxFreq)) {
            v->buf->SetFrequency(f);
            v->curFreq = f;
            calls++;
            if (s_repanAfterFreq) {                    /* 1.1: some drivers (T40 SoundMAX) lose the pan of a mono
                                                          buffer on SetFrequency: the engine loops played left only */
                v->buf->SetPan(pan_to_ds(v->pan));
                v->curPanF = v->pan;
                calls++;
            }
        }
    }
    s_masterDirty = 0;
    us = plat_now_us() - t0;
    s_st.updates++;
    s_st.calls += calls;
    s_st.lastCalls = calls;
    s_st.updUsSum += us;
    if (us > s_st.updUsMax) s_st.updUsMax = (int)us;
    s_st.voicesBusy = busy;
    s_st.voicesHw = hw;
    s_st.voicesSw = sw;
}

int au_voice_freq(AVoice h) { Voice *v = voice_of(h); return v ? (int)v->curFreq : 0; }
int au_voice_max_freq(AVoice h) { Voice *v = voice_of(h); return v ? v->maxFreq : 0; }
int au_voice_hw(AVoice h) { Voice *v = voice_of(h); return v ? v->hw : 0; }

int au_voice_status(AVoice h)
{
    DWORD st = 0;
    Voice *v = voice_of(h);
    if (!v) return -1;
    if (v->buf->GetStatus(&st) != DS_OK) return -1;
    return (st & DSBSTATUS_PLAYING) ? 1 : 0;
}

void au_stats(AuStats *out)
{
    if (out) *out = s_st;
}
