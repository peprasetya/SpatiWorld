/**
 * @file spatiworldbridge.h
 * @brief A socket on this machine through which a coding tool reads and writes the scripts and
 * notecards in your objects, and hears what they say.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */

#ifndef SPATIWORLD_BRIDGE_H
#define SPATIWORLD_BRIDGE_H

#include "llsd.h"
#include "lluuid.h"

#include <string>

// The other end is slsync, which Claude Code drives. LSL cannot read another script's source or
// write a notecard, so an object in-world could never do this; the viewer can, the way its script
// and notecard editors do.
//
// A Unix socket only this user can open: $XDG_RUNTIME_DIR/spatiworld/bridge.sock. Shut unless
// SpatiWorldBridge is set (Preferences > Firestorm > Build 1). One JSON request a line, one JSON
// answer a line, matched by "id". It only ever touches objects you own, and it never speaks on
// channel 0.
class SpatiWorldBridge
{
public:
    // Logged in: from here on it is polled from the idle loop.
    static void start();

    // What the viewer hears, kept for whoever asks (only what comes from your own objects).
    static void heardChat(const LLUUID& from_id, const LLUUID& owner_id, U8 source_type, U8 chat_type,
                          const std::string& from_name, const std::string& text);
    static void heardDialog(const LLUUID& object_id, const LLUUID& owner_id, const std::string& object_name,
                            const std::string& message, S32 channel, const LLSD& buttons);
    static void heardObjectName(const LLUUID& id, const std::string& name);
    static void heardScriptRunning(const LLUUID& object_id, const LLUUID& item_id, bool running, bool mono);
};

#endif
