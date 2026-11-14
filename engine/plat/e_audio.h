/* e_audio.h - DirectSound voices: pitch/volume/pan, batched updates (DESIGN 2.5, 8.6). Windows side.
   Owner: T08 (initial header + stub by T01).
   - dsound.dll is loaded at run time; IDirectSound / IDirectSoundBuffer only (DX3 interfaces, fine on DX 6.1).
     Cooperative level PRIORITY (primary set to rate Hz, 16-bit stereo), NORMAL if that fails. Any failure (no
     window, no dsound.dll, no driver) = silent mode: au_init returns 0 and every call is a cheap no-op (handles 0).
   - au_load keeps the PCM in memory (no DirectSound buffer). Each voice of the pool (AU_MAX_VOICES) is its own static
     secondary buffer (CTRLVOLUME | CTRLPAN | CTRLFREQUENCY), hardware-mixed when the driver has free hardware
     buffers, else software; a voice stays bound to its sound and is reused by the next au_play of that sound.
     au_play picks: a free voice of the same sound, a never-used voice, a free voice of another sound (re-created),
     else steals the oldest one-shot; looping voices are never stolen (au_play returns 0 when only loops are left).
   - vol: linear amplitude 0..1 (x au_master), DirectSound volume = 2000*log10(vol) (hundredths of dB), below -80 dB
     = DSBVOLUME_MIN. pan -1 (left) .. 1 (right): the far channel is attenuated by 2000*log10(1-|pan|). pitch = rate
     multiplier of the WAV's own rate, clamped to 100 Hz .. the buffer's maximum (hardware: DSCAPS max secondary rate,
     software: 100000 Hz).
   - au_play applies vol/pan/pitch at once. au_set only caches; au_update (once per rendered frame) commits a change
     when it exceeds 0.5 dB / 2% pan / 0.5% frequency, or reaches/leaves silence.
   - One-shot voices count as playing until their estimated end (length at the start frequency + 20 ms, by
     timeGetTime), so no per-voice driver calls are needed; au_voice_status asks the driver (tests).
   - At init the cost of each DirectSound call is measured on a silent buffer and logged
     ("snd: call_us set=.. freq=.. pan=.. ..."). */
#ifndef E_AUDIO_H
#define E_AUDIO_H

#include "e_base.h"

typedef int ASnd; typedef int AVoice;
int   au_init(void *hwnd, int rate);               /* DirectSound; 0 = silent mode, every call is a no-op */
void  au_shutdown(void);
ASnd  au_load(const char *wavPath);                /* PCM mono 8/16-bit */
AVoice au_play(ASnd s, float vol, float pan, float pitch, int loop);  /* pitch = rate multiplier */
void  au_set(AVoice v, float vol, float pan, float pitch);  /* cached; committed in au_update */
void  au_stop(AVoice v); int au_playing(AVoice v);
void  au_master(float vol);
void  au_update(void);                             /* once per rendered frame; only changed params (8.6) */

/* ---- additions (T08) ---- */
enum { AU_MAX_SOUNDS = 64, AU_MAX_VOICES = 32 };
int   au_ok(void);                                 /* 1 = DirectSound running (not silent) */
void  au_set_global_focus(int on);                 /* before au_init: keep playing when the window loses focus */
int   au_snd_rate(ASnd s);                         /* WAV sample rate, 0 if invalid */
int   au_snd_samples(ASnd s);                      /* frames */
int   au_voice_freq(AVoice v);                     /* last committed frequency (Hz), 0 if invalid */
int   au_voice_max_freq(AVoice v);                 /* clamp limit of this voice's buffer */
int   au_voice_hw(AVoice v);                       /* 1 = hardware buffer */
int   au_voice_status(AVoice v);                   /* driver GetStatus: 1 playing, 0 stopped, -1 invalid (costs a call) */
typedef struct AuCallUs { int set, freq, pan, status, pos, play, stop, create; } AuCallUs;   /* avg us per call */
typedef struct AuStats {
    int voicesBusy, voicesHw, voicesSw;            /* now */
    int plays, steals, fails, errors;              /* since init */
    int updates, calls, lastCalls;                 /* au_update count, DirectSound calls made by au_update */
    s64 updUsSum; int updUsMax;                    /* au_update time */
    int hwMaxFreq;                                 /* DSCAPS dwMaxSecondarySampleRate (0 = unknown) */
    AuCallUs callUs;                               /* measured at init */
} AuStats;
void  au_stats(AuStats *out);

#endif
