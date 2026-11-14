/* snd.h - flight sound logic over e_audio (DESIGN 15). Windows side. Owner: T08. Mix law: data/sfx/README.md (A5). */
#ifndef SND_H
#define SND_H

#include "game.h"
#include "camera.h"

void  snd_params(void);                             /* [sound] */
int   snd_init(void);
void  snd_free(void);
void  snd_set_paused(int on);
void  snd_frame(const Game *g, const Camera *c);    /* once per frame, before au_update */
void  snd_ui(int which);                           /* T08: 0 = message beep (F5 reload, mode toggle, menu) */

#endif
