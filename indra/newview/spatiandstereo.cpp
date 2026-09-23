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
#include "llquaternion.h"
#include "llviewercontrol.h"
#include "llviewerjoystick.h"
#include "llviewercamera.h"
#include "llrendertarget.h"
#include "llgl.h"
#include "llrender.h"
#include "llglslshader.h"
#include "v3math.h"

#include <cmath>
#include <cstdio>
#include <sys/socket.h>
#include "v3math.h"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/mman.h>

bool                  SpatiandStereo::sRemote = false;
SpatiandStereo::EEyes SpatiandStereo::sEyes = SpatiandStereo::EYES_MONO;
S32                   SpatiandStereo::sCurrentEye = SpatiandStereo::NO_EYE;
F32                   SpatiandStereo::sEyeSeparation = 0.064f;
const U8*             SpatiandStereo::sPoses = NULL;
LLRenderTarget*       SpatiandStereo::sEyeTarget = NULL;
LLRenderTarget*       SpatiandStereo::sUITarget = NULL;
bool                  SpatiandStereo::sUIDrawn = false;
bool                  SpatiandStereo::sFrameKnown = false;
bool                  SpatiandStereo::sTurned = false;
LLVector3             SpatiandStereo::sAimAt;
LLVector3             SpatiandStereo::sAimLeft;
LLVector3             SpatiandStereo::sAimUp;
LLVector3             SpatiandStereo::sViewAt;
LLVector3             SpatiandStereo::sViewLeft;
LLVector3             SpatiandStereo::sViewUp;
F32                   SpatiandStereo::sFrameView = 0.f;
F32                   SpatiandStereo::sFrameAspect = 0.f;
F32                   SpatiandStereo::sEyeProjection[16];
F32                   SpatiandStereo::sEyeModelview[16];
bool                  SpatiandStereo::sEyeMatricesKnown = false;
U32                   SpatiandStereo::sRenderWidth = 0;
U32                   SpatiandStereo::sRenderHeight = 0;
U32                   SpatiandStereo::sAskedWidth = 0;
U32                   SpatiandStereo::sAskedHeight = 0;
size_t                SpatiandStereo::sPosesSize = 0;

namespace
{
    // The pose ring, exactly as spatiand_xr_v1.xml lays it out and spatiand_proto::pose checks
    // it: little-endian, naturally aligned, OpenXR's frame (+X right, +Y up, -Z forward) and
    // OpenXR's field order. The asserts are the same numbers that test pins on the other side,
    // so a layout change breaks the build here rather than turning the head sideways.
    struct PoseHeader
    {
        U32 version;
        U32 slot_count;
        U32 slot_stride;
        U32 slots_offset;
        U64 write_index;
        U64 reserved;
    };

    struct PoseEye
    {
        F32 orientation[4]; // x, y, z, w
        F32 position[3];
        F32 pad;
        F32 fov[4];         // angleLeft, angleRight, angleUp, angleDown
    };

    struct PoseSlot
    {
        U64 seq;
        S64 sample_ns;
        S64 predicted_ns;
        S64 reserved;
        F32 head_orientation[4];
        F32 head_position[3];
        F32 pad;
        PoseEye eye[2];
    };

    static_assert(sizeof(PoseHeader) == 32, "pose ring header layout");
    static_assert(sizeof(PoseEye) == 48, "pose ring eye layout");
    static_assert(sizeof(PoseSlot) == 160, "pose ring slot layout");

    // A pose older than this is the headset asleep or the session gone, not a head holding
    // still -- a still head is still written every frame. Past it the view goes back to the aim
    // rather than staying turned towards wherever the wearer last looked.
    const S64 STALE_NS = 500 * 1000 * 1000;

    S64 monotonic_ns()
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (S64)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    }

    // The reader's half of the seqlock: newest slot, stepping back one if the writer is in it.
    bool read_newest(const U8* memory, PoseSlot& out)
    {
        const volatile PoseHeader* header = (const volatile PoseHeader*)memory;
        U64 written = header->write_index;
        if (written == 0)
        {
            return false;
        }
        const U32 mask = header->slot_count - 1;
        const U8* slots = memory + header->slots_offset;
        const U32 stride = header->slot_stride;
        for (U64 back = 1; back <= 2 && back <= written; ++back)
        {
            const volatile PoseSlot* at =
                (const volatile PoseSlot*)(slots + (size_t)((written - back) & mask) * stride);
            U64 before = at->seq;
            std::atomic_thread_fence(std::memory_order_acquire);
            memcpy(&out, (const void*)at, sizeof(PoseSlot));
            std::atomic_thread_fence(std::memory_order_acquire);
            U64 after = at->seq;
            if ((before & 1) == 0 && before == after)
            {
                return true;
            }
        }
        return false;
    }
}

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

    sCurrentEye = NO_EYE;

    if (sRemote)
    {
        mapPoses();
    }

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
    // The size spatiand said, if it has; otherwise twice the window as it is, which is at least
    // two eyes of the size this viewer was already drawing.
    listen();
    askForSize();

    // The glasses' own field of view, told to the simulator once. It decides what the region
    // sends, and a viewer that claims SL's sixty degrees while showing the glasses' thirty
    // gets sent twice the world it can see. Per frame the camera is held to it quietly; see
    // LLViewerCamera::updateCameraLocation.
    F32 vertical = 0.f, aspect = 0.f;
    if (eyeView(vertical, aspect))
    {
        LLViewerCamera::getInstance()->setDefaultFOV(vertical);
        LL_INFOS("Spatiand") << "drawing at the glasses' field of view: "
                             << (vertical * RAD_TO_DEG) << " degrees high, aspect " << aspect << LL_ENDL;
    }

    // The pointer arrives where the wearer sees it; the UI is laid out where the avatar aims.
    LLWindow::sCursorMap = &SpatiandStereo::mapCursor;

    // And say what this window has become: two eyes, side by side, and the room itself rather
    // than a window in it. spatiand-host passes it on; the session claims it on its side.
    tell("set_eye_layout side_by_side");
    tell("set_layer projection");
}

void SpatiandStereo::tell(const char* message)
{
    const char* fd_text = getenv("SPATIAND_CONTROL_FD");
    if (!fd_text || !fd_text[0])
    {
        LL_WARNS("Spatiand") << "cannot say '" << message << "': spatiand-host gave no control socket"
                             << " (the catalogue entry is not kind = \"vr\")" << LL_ENDL;
        return;
    }
    int fd = atoi(fd_text);
    if (send(fd, message, strlen(message), MSG_NOSIGNAL) < 0)
    {
        LL_WARNS("Spatiand") << "could not say '" << message << "': " << strerror(errno) << LL_ENDL;
        return;
    }
    LL_INFOS("Spatiand") << "told spatiand-host: " << message << LL_ENDL;
}

F32 SpatiandStereo::currentEyeShift()
{
    if (!isStereo() || sCurrentEye == NO_EYE)
    {
        return 0.f;
    }
    return (sCurrentEye > 0) ? (sEyeSeparation * 0.5f) : (-sEyeSeparation * 0.5f);
}

void SpatiandStereo::mapPoses()
{
    const char* fd_text = getenv("SPATIAND_POSE_FD");
    const char* size_text = getenv("SPATIAND_POSE_SIZE");
    if (!fd_text || !fd_text[0] || !size_text || !size_text[0])
    {
        LL_INFOS("Spatiand") << "no head to follow: spatiand-host did not hand over a pose ring"
                             << " (the catalogue entry is not kind = \"vr\")" << LL_ENDL;
        return;
    }
    int fd = atoi(fd_text);
    size_t size = (size_t)strtoull(size_text, NULL, 10);
    if (fd < 0 || size < sizeof(PoseHeader))
    {
        LL_WARNS("Spatiand") << "ignoring a pose ring that cannot be right: fd " << fd_text
                             << ", " << size_text << " bytes" << LL_ENDL;
        return;
    }
    void* memory = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (memory == MAP_FAILED)
    {
        LL_WARNS("Spatiand") << "could not map the pose ring: " << strerror(errno) << LL_ENDL;
        return;
    }
    const PoseHeader* header = (const PoseHeader*)memory;
    const U32 count = header->slot_count;
    bool power_of_two = count && !(count & (count - 1));
    if (header->version != 1 || !power_of_two || header->slot_stride < sizeof(PoseSlot)
        || (size_t)header->slots_offset + (size_t)count * header->slot_stride > size)
    {
        LL_WARNS("Spatiand") << "the pose ring is not a layout this viewer knows (version "
                             << header->version << "); not following the head" << LL_ENDL;
        munmap(memory, size);
        return;
    }
    sPoses = (const U8*)memory;
    sPosesSize = size;
    LL_INFOS("Spatiand") << "following the head: pose ring mapped, " << count << " slots" << LL_ENDL;
}

bool SpatiandStereo::headRotation(LLQuaternion& rotation)
{
    if (!sPoses)
    {
        return false;
    }
    PoseSlot slot;
    if (!read_newest(sPoses, slot))
    {
        return false;
    }
    if (monotonic_ns() - slot.sample_ns > STALE_NS)
    {
        return false;
    }
    // OpenXR's frame to the camera's. OpenXR is +X right, +Y up, -Z forward; the camera here is
    // +X at, +Y left, +Z up -- the same frame spatiand itself thinks in. A vector (a, b, c) in
    // OpenXR's is (-c, -a, b) here, a pure rotation, so the quaternion's vector part maps the
    // same way and w is untouched. This is the inverse of spatiand's to_openxr, and getting it
    // backwards turns the world ninety degrees -- which looks like a tracking bug, not a sign.
    const F32* q = slot.head_orientation;
    rotation = LLQuaternion(-q[2], -q[0], q[1], q[3]);
    rotation.normalize();
    return true;
}

void SpatiandStereo::turnByHead(LLVector3& at, LLVector3& left, LLVector3& up)
{
    LLQuaternion head;
    if (!headRotation(head))
    {
        return;
    }
    // The head's own axes, expressed in the aim's frame, then carried out into the world by the
    // aim's axes: the view is the aim with the head's turn applied on top of it.
    const LLVector3 aim_at = at;
    const LLVector3 aim_left = left;
    const LLVector3 aim_up = up;
    const LLVector3 f = LLVector3::x_axis * head;
    const LLVector3 l = LLVector3::y_axis * head;
    const LLVector3 u = LLVector3::z_axis * head;
    at   = aim_at * f.mV[VX] + aim_left * f.mV[VY] + aim_up * f.mV[VZ];
    left = aim_at * l.mV[VX] + aim_left * l.mV[VY] + aim_up * l.mV[VZ];
    up   = aim_at * u.mV[VX] + aim_left * u.mV[VY] + aim_up * u.mV[VZ];
}

S32 SpatiandStereo::flycamButton()
{
    // Right stick click on spatiand's pad, counted the way libndofdev counts: every button in
    // code order, and the right stick's is the eleventh. See spatiand-pad's BUTTON_CODES.
    static const S32 RIGHT_STICK_CLICK = 10;
    static LLCachedControl<S32> chosen(gSavedSettings, "SpatiWorldFlycamButton", -1);
    S32 button = chosen();
    if (button < 0 || button >= MAX_JOYSTICK_BUTTONS)
    {
        button = sRemote ? RIGHT_STICK_CLICK : 0;
    }
    return button;
}

bool SpatiandStereo::eyeView(F32& vertical, F32& aspect)
{
    if (!isStereo() || !sPoses)
    {
        return false;
    }
    PoseSlot slot;
    if (!read_newest(sPoses, slot))
    {
        return false;
    }
    // XrFovf: angleLeft, angleRight, angleUp, angleDown -- signed, so left and down are negative.
    const F32* fov = slot.eye[0].fov;
    const F32 up = fov[2];
    const F32 down = fov[3];
    const F32 width = tanf(fov[1]) - tanf(fov[0]);
    const F32 height = tanf(up) - tanf(down);
    if (!(up > down) || !(width > 0.f) || !(height > 0.f))
    {
        return false;
    }
    vertical = up - down;
    aspect = width / height;
    return true;
}

void SpatiandStereo::beginEye()
{
    if (!isStereo() || !gViewerWindow)
    {
        return;
    }
    const U32 width = gViewerWindow->getWindowWidthRaw();
    const U32 height = gViewerWindow->getWindowHeightRaw();
    if (!sEyeTarget)
    {
        sEyeTarget = new LLRenderTarget();
    }
    if (sEyeTarget->getWidth() != width || sEyeTarget->getHeight() != height)
    {
        sEyeTarget->release();
        // With depth: the window it stands in for has one, and the viewer draws into it as if
        // it were that window.
        if (!sEyeTarget->allocate(width, height, GL_RGBA, true))
        {
            LL_WARNS("Spatiand") << "could not make a " << width << "x" << height
                                 << " target to draw an eye into" << LL_ENDL;
            return;
        }
        LL_INFOS("Spatiand") << "drawing each eye at " << width << "x" << height << LL_ENDL;
    }
    // As the bottom of the viewer's own stack of render targets, so that every target it binds
    // and flushes during the frame hands back to this one rather than to the window.
    sEyeTarget->bindTarget();
    sEyeTarget->clear();
}

void SpatiandStereo::endEye()
{
    if (!sEyeTarget || LLRenderTarget::getCurrentBoundTarget() != sEyeTarget)
    {
        return;
    }
    const S32 width = sEyeTarget->getWidth();
    const S32 height = sEyeTarget->getHeight();
    sEyeTarget->flush();
    // Into its half of the window: the left eye at the left edge, the right one beside it.
    const S32 x = sCurrentEye > 0 ? width : 0;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sEyeTarget->getFBO());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, width, height, x, 0, x + width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SpatiandStereo::present()
{
    if (isStereo() && gViewerWindow && gViewerWindow->getWindow())
    {
        gViewerWindow->getWindow()->swapBuffers();
    }
}

void SpatiandStereo::listen()
{
    static const char* fd_text = getenv("SPATIAND_CONTROL_FD");
    if (!fd_text || !fd_text[0])
    {
        return;
    }
    static const int fd = atoi(fd_text);
    char buffer[128];
    for (;;)
    {
        ssize_t n = recv(fd, buffer, sizeof(buffer) - 1, MSG_DONTWAIT);
        if (n <= 0)
        {
            break;
        }
        buffer[n] = 0;
        unsigned width = 0, height = 0;
        if (sscanf(buffer, "set_render_size %u %u", &width, &height) == 2 && width > 0 && height > 0)
        {
            if (width != sRenderWidth || height != sRenderHeight)
            {
                sRenderWidth = width;
                sRenderHeight = height;
                LL_INFOS("Spatiand") << "spatiand wants the picture at " << width << "x" << height << LL_ENDL;
            }
        }
        else
        {
            LL_WARNS("Spatiand") << "spatiand-host said something this viewer does not know: " << buffer << LL_ENDL;
        }
    }
    if (isStereo())
    {
        askForSize();
    }
}

void SpatiandStereo::askForSize()
{
    if (!isStereo() || !gViewerWindow || !gViewerWindow->getWindow())
    {
        return;
    }
    U32 width = sRenderWidth;
    U32 height = sRenderHeight;
    if (!width || !height)
    {
        // Nothing said yet: two eyes the size of the window as it was.
        if (sAskedWidth)
        {
            return;
        }
        width = gViewerWindow->getWindowWidthRaw() * 2;
        height = gViewerWindow->getWindowHeightRaw();
    }
    // Asked once per size, not every frame: a resize takes a round trip through the compositor,
    // and asking again meanwhile would only queue more of them.
    if (width == sAskedWidth && height == sAskedHeight)
    {
        return;
    }
    sAskedWidth = width;
    sAskedHeight = height;
    LL_INFOS("Spatiand") << "asking for a window of " << width << "x" << height
                         << " to put two " << (width / 2) << "x" << height << " eyes in" << LL_ENDL;
    gViewerWindow->getWindow()->setSize(LLCoordWindow(width, height));
}

void SpatiandStereo::beginFrame()
{
    sTurned = false;
    if (!isStereo())
    {
        return;
    }
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    sAimAt = camera->getAtAxis();
    sAimLeft = camera->getLeftAxis();
    sAimUp = camera->getUpAxis();
    sViewAt = sAimAt;
    sViewLeft = sAimLeft;
    sViewUp = sAimUp;
    turnByHead(sViewAt, sViewLeft, sViewUp);
    sFrameView = camera->getView();
    sFrameAspect = camera->getAspect();
    camera->setAxes(sViewAt, sViewLeft, sViewUp);
    sTurned = true;
    sFrameKnown = true;
}

void SpatiandStereo::endFrame()
{
    if (!sTurned)
    {
        return;
    }
    sTurned = false;
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    camera->setAxes(sAimAt, sAimLeft, sAimUp);
    // And the matrices that code outside drawing projects through -- a floater opened beside
    // an object, a drag -- to the aim's, from the middle of the head, as setPerspective builds
    // them. Otherwise they would still be the last eye's, turned by the head.
    glm::mat4 modelview(glm::make_mat4((GLfloat*) OGL_TO_CFR_ROTATION));
    GLfloat transform[16];
    camera->getOpenGLTransform(transform);
    modelview *= glm::make_mat4(transform);
    set_current_modelview(modelview);
}

bool SpatiandStereo::mapCursor(S32& x, S32& y, bool to_layout)
{
    if (!isStereo() || !sFrameKnown || !gViewerWindow)
    {
        return false;
    }
    const F32 width = (F32)gViewerWindow->getWindowWidthRaw();
    const F32 height = (F32)gViewerWindow->getWindowHeightRaw();
    if (width <= 0.f || height <= 0.f || sFrameView <= 0.f || sFrameAspect <= 0.f)
    {
        return false;
    }
    // A pixel is a direction through the field of view; the same direction, seen from the
    // other frame, is the pixel wanted. Both frames share the field of view and the middle of
    // the head, so this is a turn and nothing else -- which is also why a click past the edge
    // of the panel still casts the right ray into the world.
    const LLVector3& from_at   = to_layout ? sViewAt : sAimAt;
    const LLVector3& from_left = to_layout ? sViewLeft : sAimLeft;
    const LLVector3& from_up   = to_layout ? sViewUp : sAimUp;
    const LLVector3& to_at     = to_layout ? sAimAt : sViewAt;
    const LLVector3& to_left   = to_layout ? sAimLeft : sViewLeft;
    const LLVector3& to_up     = to_layout ? sAimUp : sViewUp;
    const F32 tan_v = tanf(sFrameView * 0.5f);
    const F32 tan_h = tan_v * sFrameAspect;
    // Window coordinates: x from the left, y from the top.
    const F32 tx = (((F32)x + 0.5f) / width * 2.f - 1.f) * tan_h;
    const F32 ty = (1.f - ((F32)y + 0.5f) / height * 2.f) * tan_v;
    const LLVector3 direction = from_at - from_left * tx + from_up * ty;
    const F32 along = direction * to_at;
    if (along <= 1e-4f)
    {
        // Behind the other frame: there is no pixel of it there.
        return false;
    }
    const F32 ux = -(direction * to_left) / along / tan_h;
    const F32 uy = (direction * to_up) / along / tan_v;
    x = ll_round((ux + 1.f) * 0.5f * width - 0.5f);
    y = ll_round((1.f - uy) * 0.5f * height - 0.5f);
    return true;
}

void SpatiandStereo::noteEyeMatrices(const F32* projection, const F32* modelview)
{
    if (!isStereo() || sCurrentEye == NO_EYE)
    {
        return;
    }
    memcpy(sEyeProjection, projection, sizeof(sEyeProjection));
    memcpy(sEyeModelview, modelview, sizeof(sEyeModelview));
    sEyeMatricesKnown = true;
}

bool SpatiandStereo::beginUI()
{
    // Once a frame: the panel is the same for both eyes, and drawing it twice would also run
    // everything in the UI that counts frames twice (the HUD's zoom easing, for one).
    if (!isStereo() || sCurrentEye > 0 || !gViewerWindow)
    {
        return false;
    }
    const U32 width = gViewerWindow->getWindowWidthRaw();
    const U32 height = gViewerWindow->getWindowHeightRaw();
    if (!sUITarget)
    {
        sUITarget = new LLRenderTarget();
    }
    if (sUITarget->getWidth() != width || sUITarget->getHeight() != height)
    {
        sUITarget->release();
        // With depth, for the HUD attachments: they are objects, and sort by it.
        if (!sUITarget->allocate(width, height, GL_RGBA, true))
        {
            LL_WARNS("Spatiand") << "could not make a " << width << "x" << height
                                 << " target to lay the UI out in" << LL_ENDL;
            return false;
        }
    }
    sUITarget->bindTarget();
    // Alpha written too: it is what lets the world show through wherever there is no UI.
    gGL.setColorMask(true, true);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    sUITarget->clear();
    return true;
}

void SpatiandStereo::endUI()
{
    gGL.flush();
    sUITarget->flush();
    gGL.setColorMask(true, false);
    sUIDrawn = true;
}

void SpatiandStereo::drawUI()
{
    if (!sUIDrawn || !sUITarget || !sEyeMatricesKnown || !sFrameKnown)
    {
        return;
    }
    // How far in front of the aim the panel stands. Its size follows, so it fills the aim's
    // view exactly; the distance only decides where the eyes converge on it. Nearer is where a
    // screen is read from; farther agrees better with spatiand's pointer, which is drawn at
    // the depth of the room and is double against anything much nearer.
    static LLCachedControl<F32> panel_distance(gSavedSettings, "SpatiWorldPanelDistance", 4.f);
    const F32 PANEL_DISTANCE = llclamp((F32)panel_distance, 0.5f, 100.f);
    const F32 tan_v = tanf(sFrameView * 0.5f);
    const F32 tan_h = tan_v * sFrameAspect;
    const LLVector3 centre = LLViewerCamera::getInstance()->getOrigin() + sAimAt * PANEL_DISTANCE;
    const LLVector3 right = -sAimLeft * (PANEL_DISTANCE * tan_h);
    const LLVector3 up = sAimUp * (PANEL_DISTANCE * tan_v);
    const LLVector3 bottom_left = centre - right - up;
    const LLVector3 bottom_right = centre + right - up;
    const LLVector3 top_left = centre - right + up;
    const LLVector3 top_right = centre + right + up;

    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.loadMatrix(sEyeProjection);
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    gGL.loadMatrix(sEyeModelview);

    {
        LLGLDepthTest no_depth(GL_FALSE, GL_FALSE);
        LLGLDisable no_cull(GL_CULL_FACE);
        LLGLEnable blend(GL_BLEND);
        // What the UI drew into a clear target is already multiplied by its own alpha.
        gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);
        gUIProgram.bind();
        gGL.getTexUnit(0)->bind(sUITarget);
        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
        gGL.color4f(1.f, 1.f, 1.f, 1.f);
        gGL.begin(LLRender::TRIANGLE_STRIP);
        gGL.texCoord2f(0.f, 0.f); gGL.vertex3fv(bottom_left.mV);
        gGL.texCoord2f(1.f, 0.f); gGL.vertex3fv(bottom_right.mV);
        gGL.texCoord2f(0.f, 1.f); gGL.vertex3fv(top_left.mV);
        gGL.texCoord2f(1.f, 1.f); gGL.vertex3fv(top_right.mV);
        gGL.end();
        gGL.flush();
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gUIProgram.unbind();
        gGL.setSceneBlendType(LLRender::BT_ALPHA);
    }

    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
}
