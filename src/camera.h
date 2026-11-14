/* camera.h - cockpit + chase cameras, wreck camera, fixed -camat camera (DESIGN 7). Windows/render side (reads
   interpolated state only, never writes game state). Owner: T08.

   Cockpit (HUD-only view): eye at the jet's cam_cockpit tag (0, 0.98, 4.35), orientation = qNose exactly, so the
   screen centre is the boresight = HUD waterline and the velocity vector sits alpha below it (T07 rule). Near 1.0 m.
   Chase: eye chaseDist behind / chaseUp above along a LAGGING camera frame (forward smoothed with tauCamRot, up with
   tauCamRoll), looking at the jet + nose * chaseLook, so the jet visibly leads the camera in manoeuvres. FOV fovChase
   + fovAB x afterburner. Near 1.5 m. The eye stays chaseMinAgl above the surface. Small shake above 7 g / in AB.
   Wreck: while the player is dead (crash), both views show a slow orbit around the wreck.
   -camat X Y Z HDG PITCH: a fixed camera there (screenshots); the HUD is hidden.
   Projection: horizontal FOV at the screen aspect (65 deg cockpit at 4:3 = the HUD's scale). Far = scene_far_m(). */
#ifndef CAMERA_H
#define CAMERA_H

#include "game.h"

typedef struct Camera {
    int mode;                                       /* VIEW_COCKPIT / VIEW_CHASE */
    Vec3 eye; Quat q;                               /* world position, orientation (+Z forward) */
    float fovDeg, nearM, farM;                      /* horizontal FOV */
    Mat4 view, proj;
    /* ---- T08 ---- */
    Vec3 fwd, up;                                   /* chase: lagging camera frame */
    int snap;                                       /* 1 = next update jumps (no smoothing): view switch, respawn */
    int wreck;                                      /* 1 = the wreck orbit is shown */
    float wreckT;                                   /* seconds in the wreck orbit */
    int fixed;                                      /* -camat */
    Vec3 fixedEye; float fixedHdg, fixedPitch;      /* degrees */
    u32 shakeSeed;
    /* ---- T10b: speed cues ---- */
    float fovSpeedK, fovAbK;                        /* smoothed speed FOV fraction, smoothed afterburner for the FOV */
    float buffetDeg, buffetT;                       /* buffet amplitude this frame, buffet clock (s) */
} Camera;

void  cam_params(void);                             /* [camera] */
void  cam_reset(Camera *c, const Aircraft *ac);     /* start view from [camera] startView; snaps */
void  cam_toggle(Camera *c);
void  cam_update(Camera *c, const Aircraft *ac, float alpha, float frameDt);

/* ---- T08 ---- */
void  cam_set_fixed(Camera *c, Vec3 eye, float hdgDeg, float pitchDeg);   /* -camat */
/* interpolated pose of an aircraft for rendering: position and qNose = slerp(qPath) * rotX(-alpha) */
void  cam_ac_pose(const Aircraft *a, float alpha, Vec3 *pos, Quat *qNose);
void  cam_frustum(const Camera *c, Frustum *fr);    /* from view * proj */

#endif
