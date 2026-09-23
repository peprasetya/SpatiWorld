/**
 * @file spatiandstereo.h
 * @brief Whether spatiand is watching, and whether it wants two eyes.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * SpatiWorld Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#ifndef SPATIANDSTEREO_H
#define SPATIANDSTEREO_H

#include "stdtypes.h"

#include <cstddef>

class LLQuaternion;
class LLVector3;
class LLRenderTarget;

// Run as a remote application of a spatiand host, this viewer may be asked to draw both of the
// wearer's eyes into one window. Being remote and being in a headset are asked separately on
// purpose: the same protocol is meant to carry an ordinary window to an ordinary flat desktop,
// and a viewer that read "spatiand is there" as "draw two eyes" would be wrong the day that
// happens.
class SpatiandStereo
{
public:
    enum EEyes
    {
        EYES_MONO,
        EYES_SIDE_BY_SIDE,
        EYES_TOP_BOTTOM,
    };

    // Read what spatiand-host launched us with. Called once the world is up, never before: the
    // login window stays an ordinary flat window whatever ends up watching it.
    static void detect();

    static bool isRemote() { return sRemote; }
    static EEyes eyes() { return sEyes; }
    // Only side-by-side is drawn so far. Top/bottom is a value the protocol has and this
    // viewer does not yet honour, so it stays one eye rather than drawing the wrong thing.
    static bool isStereo() { return sEyes == EYES_SIDE_BY_SIDE; }

    static S32 eyeCount() { return isStereo() ? 2 : 1; }
    // Which eye is being drawn: 0 left, 1 right, NO_EYE between passes. Between passes the
    // camera is the middle of the head, so whatever projects through it outside drawing (a
    // pick, a name tag's placement) is not off to one side.
    enum { NO_EYE = -1 };
    static S32 currentEye() { return sCurrentEye; }
    static void setEye(S32 eye) { sCurrentEye = eye; }

    // **Each eye in a framebuffer of its own.** An eye is drawn into an offscreen target the
    // size of one eye, at 0,0, and only then copied into its half of the window. Drawing it
    // straight into its half with an offset viewport leaked at every seam: the UI's scissor is
    // in window coordinates, whole-window clears wiped the other eye, a resize re-fed the
    // halved width. To the viewer each pass is an ordinary single-window frame, and nothing in
    // it has to know there are two.
    static void beginEye();
    static void endEye();
    // Show the frame: one swap, after both eyes are in the window.
    static void present();

    // Read what spatiand-host has said, and act on it. It says one thing: the size the session
    // wants the two eyes drawn at, which is the glasses' own and not whatever this window
    // happened to be. Called once a frame, before and after login alike; one non-blocking read.
    static void listen();

    // How far this eye sits from the middle of the head, in metres: negative is left of it.
    static F32 currentEyeShift();
    static F32 eyeSeparation() { return sEyeSeparation; }

    // **Where the head is turned, relative to where the avatar is aiming.** In the camera's own
    // frame: +X along the aim, +Y to its left, +Z up. The view is the aim turned by this; the
    // aim itself -- what the thumbsticks steer and the avatar follows -- is left alone, so the
    // wearer can look around without changing where they are going.
    //
    // Read from the shared-memory ring spatiand-host hands this process, newest sample, taken
    // the moment it is asked for. False when there is no head to follow: no ring, nothing in it
    // yet, or nothing recent (the headset asleep, the session gone). Then the view is the aim,
    // which is what an ordinary flat viewer does.
    static bool headRotation(LLQuaternion& rotation);

    // Turn an aim -- its three axes -- by the wearer's head, in place, so they become the view.
    // Left exactly as they were when there is no head to follow.
    static void turnByHead(LLVector3& at, LLVector3& left, LLVector3& up);

    // **The camera is the aim, except while an eye is being drawn.** Between frames
    // LLViewerCamera points where the avatar aims, or where the flycam does: the sticks steer
    // it, picks and hovers are cast through it, the UI is laid out in front of it. Only for
    // the passes that draw the eyes is it turned by the head, and then put back. So the head
    // changes what is seen and nothing else, and a click lands where it was laid out, which
    // mapCursor makes the same place it was seen.
    static void beginFrame();
    static void endFrame();

    // Map the pointer between the view that is shown and the aim the UI is laid out in:
    // LLWindow::sCursorMap while drawing two eyes. Window coordinates, one eye wide.
    static bool mapCursor(S32& x, S32& y, bool to_layout);

    // **The UI is a panel in front of the aim.** Menus, floaters and HUD attachments are drawn
    // once a frame into a target of their own, laid out exactly as on a flat screen, and that
    // target is set in the world in front of the aim, filling the view the aim has. Each eye
    // then sees it where it stands, with the head free to look away from it and back -- so a
    // button in a corner is reached by turning the head a little, not by straining the eyes.
    // beginUI is true once a frame, in the first eye, with the panel's target bound; drawUI
    // puts the panel into whichever eye is being drawn.
    static bool beginUI();
    static void endUI();
    static void drawUI();
    // The eye's own matrices, as LLViewerCamera::setPerspective made them, to draw the panel with.
    static void noteEyeMatrices(const F32* projection, const F32* modelview);

    // The joystick button that detaches the camera from the avatar, and gives it back.
    //
    // Second Life hard-wires this to button 0, which on a 3D mouse is the left button and on a
    // gamepad is A -- the button a thumb rests on to jump or confirm. Under spatiand the
    // default is the right stick's click instead: the right stick is already the camera, so
    // pressing it to set the camera free is where a hand expects it to be. SpatiWorldFlycamButton
    // overrides either; -1 means "whichever of those fits".
    static S32 flycamButton();

    // The glasses' field of view, as the camera wants it: the vertical angle in radians and
    // the width-to-height ratio. What the viewer draws has to be exactly what the glasses show,
    // or a turn of the head moves the world by the wrong amount and it swims. False when not
    // drawing two eyes, or before the first pose has arrived.
    static bool eyeView(F32& vertical, F32& aspect);

private:
    static bool  sRemote;
    static EEyes sEyes;
    static S32   sCurrentEye;
    static F32   sEyeSeparation;

    static void mapPoses();
    static LLRenderTarget* sEyeTarget;
    static LLRenderTarget* sUITarget;
    static bool sUIDrawn;
    // The aim and the view this frame was drawn with, and its field of view.
    static bool sFrameKnown;
    static bool sTurned;
    static LLVector3 sAimAt, sAimLeft, sAimUp;
    static LLVector3 sViewAt, sViewLeft, sViewUp;
    static F32 sFrameView;
    static F32 sFrameAspect;
    static F32 sEyeProjection[16];
    static F32 sEyeModelview[16];
    static bool sEyeMatricesKnown;
    // What spatiand-host last said the picture should be, both eyes together, and what this
    // viewer last asked its window to become.
    static U32 sRenderWidth;
    static U32 sRenderHeight;
    static U32 sAskedWidth;
    static U32 sAskedHeight;
    static void askForSize();
    // Say something to spatiand-host down the control socket, in spatiand_xr_v1's words.
    static void tell(const char* message);
    static const U8* sPoses;
    static size_t    sPosesSize;
};

#endif // SPATIANDSTEREO_H
