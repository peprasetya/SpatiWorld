/**
 * @file spatiandstereo.cpp
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

#include "llviewerprecompiledheaders.h"

#include "spatiandstereo.h"

#include "llviewerwindow.h"
#include "llwindow.h"

#include <cstdlib>
#include <cstring>

bool                  SpatiandStereo::sRemote = false;
SpatiandStereo::EEyes SpatiandStereo::sEyes = SpatiandStereo::EYES_MONO;
S32                   SpatiandStereo::sCurrentEye = 0;
F32                   SpatiandStereo::sEyeSeparation = 0.064f;

namespace
{
    // An unset variable and an empty one mean the same thing here: nobody said.
    const char* said(const char* name)
    {
        const char* value = getenv(name);
        return (value && value[0]) ? value : NULL;
    }
}

void SpatiandStereo::detect()
{
    sRemote = (said("SPATIAND_REMOTE_APP") != NULL);

    sEyes = EYES_MONO;
    if (sRemote)
    {
        const char* eyes = said("SPATIAND_EYES");
        if (eyes)
        {
            if (!strcmp(eyes, "side_by_side"))
            {
                sEyes = EYES_SIDE_BY_SIDE;
            }
            else if (!strcmp(eyes, "top_bottom"))
            {
                sEyes = EYES_TOP_BOTTOM;
            }
        }
    }

    // The wearer's own separation, once the host knows how to measure it. Until then every
    // head is the population average, which is wrong for everybody by a millimetre or two and
    // is still better than guessing per viewer.
    if (const char* sep = said("SPATIAND_EYE_SEPARATION"))
    {
        F32 metres = (F32)atof(sep);
        if (metres > 0.f && metres < 0.2f)
        {
            sEyeSeparation = metres;
        }
        else
        {
            LL_WARNS("Spatiand") << "ignoring SPATIAND_EYE_SEPARATION=" << sep
                                 << ", which is not a plausible distance between two eyes" << LL_ENDL;
        }
    }

    sCurrentEye = 0;

    if (!sRemote)
    {
        LL_INFOS("Spatiand") << "not a spatiand remote application; drawing one eye" << LL_ENDL;
        return;
    }

    LL_INFOS("Spatiand") << "spatiand remote application '" << getenv("SPATIAND_REMOTE_APP")
                         << "', eyes=" << (sEyes == EYES_SIDE_BY_SIDE ? "side_by_side"
                                        : sEyes == EYES_TOP_BOTTOM   ? "top_bottom" : "mono")
                         << LL_ENDL;

    if (sEyes == EYES_TOP_BOTTOM)
    {
        LL_WARNS("Spatiand") << "top/bottom packing is not drawn yet; staying on one eye" << LL_ENDL;
        return;
    }

    if (!isStereo() || !gViewerWindow || !gViewerWindow->getWindow())
    {
        return;
    }

    // **Twice as wide, not half the eye.** The window keeps the width it has been laid out for
    // and gains a second copy of it beside itself, so each eye is drawn at the resolution the
    // one flat window had rather than at half of it. Everything inside the viewer goes on
    // believing the window is one eye wide — see LLViewerWindow::reshape, which halves what
    // the compositor hands back.
    S32 eye_width = gViewerWindow->getWindowWidthRaw();
    S32 height = gViewerWindow->getWindowHeightRaw();
    LL_INFOS("Spatiand") << "asking for a window of " << (eye_width * 2) << "x" << height
                         << " to put two " << eye_width << "x" << height << " eyes in" << LL_ENDL;
    gViewerWindow->getWindow()->setSize(LLCoordWindow(eye_width * 2, height));
}

F32 SpatiandStereo::currentEyeShift()
{
    if (!isStereo())
    {
        return 0.f;
    }
    return (sCurrentEye > 0) ? (sEyeSeparation * 0.5f) : (-sEyeSeparation * 0.5f);
}
