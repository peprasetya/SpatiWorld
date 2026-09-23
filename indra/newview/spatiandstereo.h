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
    static S32 currentEye() { return sCurrentEye; }
    static void setCurrentEye(S32 eye) { sCurrentEye = eye; }
    static bool isLastEye() { return sCurrentEye >= eyeCount() - 1; }

    // Where this eye's half of the window starts, in raw pixels from the left edge. Takes the
    // width of one eye rather than asking the window for it, so that the viewport code can
    // call this while it is in the middle of deciding what the window is.
    static S32 viewportOffsetX(S32 eye_width)
    {
        return (isStereo() && sCurrentEye > 0) ? eye_width : 0;
    }

    // How far this eye sits from the middle of the head, in metres: negative is left of it.
    static F32 currentEyeShift();
    static F32 eyeSeparation() { return sEyeSeparation; }

private:
    static bool  sRemote;
    static EEyes sEyes;
    static S32   sCurrentEye;
    static F32   sEyeSeparation;
};

#endif // SPATIANDSTEREO_H
