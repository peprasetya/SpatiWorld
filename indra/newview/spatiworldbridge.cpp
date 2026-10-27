/**
 * @file spatiworldbridge.cpp
 * @brief A socket on this machine through which a coding tool reads and writes the scripts and
 * notecards in your objects, and hears what they say. See spatiworldbridge.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "spatiworldbridge.h"

#include "fscommon.h"
#include "llagent.h"
#include "llagentui.h"
#include "llchat.h"
#include "llassetstorage.h"
#include "llcallbacklist.h"
#include "llexperiencecache.h"
#include "llfilesystem.h"
#include "llfloaterperms.h"
#include "llinventorydefines.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llnotecard.h"
#include "llsdjson.h"
#include "llselectmgr.h"
#include "llstartup.h"
#include "lltooldraganddrop.h"
#include "lltoolgrab.h"
#include "lltoolmgr.h"
#include "lltoolpie.h"
#include "llviewerassettype.h"
#include "llviewerassetupload.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerjointattachment.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "llvoinventorylistener.h"
#include "llvolumemessage.h"
#include "llworld.h"
#include "material_codes.h"
#include "message.h"
#include "roles_constants.h"
#include "spatiandstereo.h"

#include <boost/json.hpp>

#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <set>

#if LL_LINUX || LL_DARWIN
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#define SPATIWORLD_BRIDGE_SOCKET 1
#endif

namespace
{

constexpr S32 BRIDGE_VERSION = 1;
// A request carries a whole script at most (64 KB of source in SL), so anything far past that is
// not a request.
constexpr size_t MAX_LINE = 4 * 1024 * 1024;
// What is kept of what your objects said, for whoever asks next.
constexpr size_t EVENTS_KEPT = 2000;

F64 now()
{
    return LLTimer::getElapsedSeconds();
}

//-------------------------------------------------------------------------------------------------
// Who asked, and the answer, later.

struct Client
{
    int fd = -1;
    std::string in;
    std::string out;
};

std::map<U32, Client> sClients;
U32 sNextClient = 1;
int sListenFd = -1;
F64 sNextOpenTry = 0.0;

struct Asker
{
    U32 client = 0;
    LLSD id;
};

void answer(const Asker& asker, LLSD reply)
{
    auto it = sClients.find(asker.client);
    if (it == sClients.end())
    {
        return; // Gone meanwhile: nobody to tell.
    }
    reply["id"] = asker.id;
    if (!reply.has("ok"))
    {
        reply["ok"] = true;
    }
    it->second.out += boost::json::serialize(LlsdToJson(reply));
    it->second.out += '\n';
}

void fail(const Asker& asker, const std::string& why)
{
    LLSD reply;
    reply["ok"] = false;
    reply["error"] = why;
    answer(asker, reply);
}

// Things that answer once something has arrived from the region, or once it is too late to.
struct Wait
{
    F64 deadline;
    std::function<bool()> ready;
    std::function<void(bool timed_out)> then;
};
std::list<Wait> sWaits;

void waitFor(F32 seconds, std::function<bool()> ready, std::function<void(bool)> then)
{
    sWaits.push_back({ now() + seconds, std::move(ready), std::move(then) });
}

void runWaits()
{
    // A `then` may add waits of its own; they go on the end and are looked at next frame or
    // later in this pass, either is fine.
    for (auto it = sWaits.begin(); it != sWaits.end();)
    {
        bool done = it->ready();
        bool late = !done && now() > it->deadline;
        if (done || late)
        {
            auto then = std::move(it->then);
            it = sWaits.erase(it);
            then(late);
        }
        else
        {
            ++it;
        }
    }
}

//-------------------------------------------------------------------------------------------------
// What the viewer hears.

// The last few seconds of frame times, to say how smoothly the viewer is drawing.
std::deque<std::pair<F64, F64>> sFrames; // when, how long since the one before

void noteFrame()
{
    static F64 last = 0.0;
    const F64 t = now();
    if (last > 0.0)
    {
        sFrames.emplace_back(t, t - last);
    }
    last = t;
    while (!sFrames.empty() && t - sFrames.front().first > 5.0)
    {
        sFrames.pop_front();
    }
}

std::deque<LLSD> sEvents;
S64 sNextEvent = 1;

void keepEvent(LLSD event)
{
    event["seq"] = (LLSD::Integer)sNextEvent++;
    event["time"] = LLDate::now();
    sEvents.push_back(event);
    while (sEvents.size() > EVENTS_KEPT)
    {
        sEvents.pop_front();
    }
}

struct Named
{
    std::string name;
    F64 heard = 0.0;
};
std::map<LLUUID, Named> sNames;
std::map<LLUUID, F64> sNamesAsked;
std::map<LLUUID, F64> sNamesSelected;

// The region does not answer RequestObjectPropertiesFamily for the prims of a worn HUD. Selecting
// is answered for anything you own (it is how the edit tool learns names), and a select taken
// straight back again is what Area Search does. Never for something you have selected yourself:
// the deselect would take it out of your selection on the region.
void selectForName(LLViewerObject* object)
{
    if (LLSelectMgr::getInstance()->getSelection()->findNode(object))
    {
        return;
    }
    for (const char* message : { _PREHASH_ObjectSelect, _PREHASH_ObjectDeselect })
    {
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(message);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_ObjectLocalID, object->getLocalID());
        msg->sendReliable(object->getRegion()->getHost());
    }
}

struct Running
{
    bool running = false;
    bool mono = true;
    U32 serial = 0; // bumped on every reply, so a wait can tell a fresh one
};
std::map<std::pair<LLUUID, LLUUID>, Running> sRunning;
U32 sRunningSerial = 0;

//-------------------------------------------------------------------------------------------------
// Objects.

LLViewerObject* ownedObject(const LLUUID& id, std::string& why)
{
    LLViewerObject* object = gObjectList.findObject(id);
    if (!object || object->isDead())
    {
        why = "no object " + id.asString() + " here: it has to be in this viewer's view, or worn";
        return nullptr;
    }
    if (object->isAvatar() || !object->permYouOwner())
    {
        why = "object " + id.asString() + " is not yours";
        return nullptr;
    }
    if (!object->getRegion())
    {
        why = "object " + id.asString() + " has no region";
        return nullptr;
    }
    return object;
}

void linkset(LLViewerObject* root, std::vector<LLPointer<LLViewerObject>>& prims)
{
    prims.push_back(root);
    for (const LLPointer<LLViewerObject>& child : root->getChildren())
    {
        if (child && !child->isAvatar() && !child->isDead())
        {
            prims.push_back(child);
        }
    }
}

std::string attachmentPointName(LLViewerObject* object, S32* point_out = nullptr)
{
    if (!object->isAttachment() || !isAgentAvatarValid())
    {
        return std::string();
    }
    S32 point = ATTACHMENT_ID_FROM_STATE(object->getAttachmentState());
    if (point_out)
    {
        *point_out = point;
    }
    auto it = gAgentAvatarp->mAttachmentPoints.find(point);
    return it != gAgentAvatarp->mAttachmentPoints.end() && it->second ? it->second->getName() : std::string();
}

std::string nameOf(LLViewerObject* object)
{
    auto it = sNames.find(object->getID());
    if (it != sNames.end())
    {
        return it->second.name;
    }
    if (object->isAttachment() && object->isRootEdit())
    {
        return object->getAttachmentItemName();
    }
    return std::string();
}

// The region names objects only when asked. Asked again after a while, in case one was renamed.
bool askNames(const std::vector<LLPointer<LLViewerObject>>& objects)
{
    bool all = true;
    const F64 t = now();
    for (const LLPointer<LLViewerObject>& object : objects)
    {
        auto heard = sNames.find(object->getID());
        if (heard != sNames.end() && t - heard->second.heard < 30.0)
        {
            continue;
        }
        all = false;
        auto asked = sNamesAsked.find(object->getID());
        if (asked != sNamesAsked.end() && t - asked->second < 5.0)
        {
            // Asked, and not answered: worn, most likely. Ask the other way, once a while.
            auto selected = sNamesSelected.find(object->getID());
            if ((object->isAttachment() || t - asked->second > 1.5)
                && (selected == sNamesSelected.end() || t - selected->second > 5.0))
            {
                sNamesSelected[object->getID()] = t;
                selectForName(object);
            }
            continue;
        }
        sNamesAsked[object->getID()] = t;
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_RequestObjectPropertiesFamily);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_RequestFlags, 0x0);
        msg->addUUIDFast(_PREHASH_ObjectID, object->getID());
        msg->sendReliable(object->getRegion()->getHost());
    }
    return all;
}

LLSD triple(const LLVector3& v)
{
    LLSD out = LLSD::emptyArray();
    out.append(ll_round(v.mV[VX] * 1000.f) / 1000.0);
    out.append(ll_round(v.mV[VY] * 1000.f) / 1000.0);
    out.append(ll_round(v.mV[VZ] * 1000.f) / 1000.0);
    return out;
}

LLSD describe(LLViewerObject* root)
{
    LLSD out;
    out["id"] = root->getID();
    out["name"] = nameOf(root);
    out["region"] = root->getRegion()->getName();
    std::vector<LLPointer<LLViewerObject>> prims;
    linkset(root, prims);
    out["prims"] = (LLSD::Integer)prims.size();
    std::string point = attachmentPointName(root);
    if (root->isAttachment())
    {
        out["attached"] = point;
        out["hud"] = root->isHUDAttachment();
    }
    else
    {
        out["position"] = triple(root->getPositionRegion());
        out["distance"] = ll_round(dist_vec(root->getPositionGlobal(), gAgent.getPositionGlobal()) * 10.0) / 10.0;
    }
    return out;
}

void ownedRoots(std::vector<LLPointer<LLViewerObject>>& roots)
{
    for (S32 i = 0; i < gObjectList.getNumObjects(); ++i)
    {
        LLViewerObject* object = gObjectList.getObject(i);
        if (!object || object->isDead() || object->isAvatar() || !object->getRegion())
        {
            continue;
        }
        if (!object->isRootEdit() || !object->permYouOwner())
        {
            continue;
        }
        if (object->isAttachment() && object->getAvatar() != gAgentAvatarp)
        {
            continue;
        }
        roots.push_back(object);
    }
}

//-------------------------------------------------------------------------------------------------
// A prim's contents, fetched afresh: a listing kept from before can name an asset that has since
// been replaced, and the asset store would then hand back the old text from its cache.

class ContentsWait : public LLVOInventoryListener
{
public:
    explicit ContentsWait(LLViewerObject* object)
    {
        registerVOInventoryListener(object, nullptr);
        object->dirtyInventory();
        requestVOInventory();
    }
    void inventoryChanged(LLViewerObject*, LLInventoryObject::object_list_t*, S32, void*) override
    {
        mDone = true;
    }
    bool mDone = false;
};

bool isWanted(const LLInventoryItem* item)
{
    return item && (item->getType() == LLAssetType::AT_LSL_TEXT || item->getType() == LLAssetType::AT_NOTECARD);
}

LLSD itemList(LLViewerObject* prim)
{
    LLSD items = LLSD::emptyArray();
    LLInventoryObject::object_list_t contents;
    prim->getInventoryContents(contents);
    for (const LLPointer<LLInventoryObject>& entry : contents)
    {
        const LLInventoryItem* item = dynamic_cast<const LLInventoryItem*>(entry.get());
        if (!isWanted(item) || item->getUUID().isNull())
        {
            continue;
        }
        LLSD out;
        out["id"] = item->getUUID();
        out["name"] = item->getName();
        out["type"] = item->getType() == LLAssetType::AT_LSL_TEXT ? "script" : "notecard";
        out["asset"] = item->getAssetUUID();
        const LLPermissions& perm = item->getPermissions();
        out["modify"] = gAgent.allowOperation(PERM_MODIFY, perm, GP_OBJECT_MANIPULATE);
        out["copy"] = gAgent.allowOperation(PERM_COPY, perm, GP_OBJECT_MANIPULATE);
        out["created"] = (LLSD::Integer)item->getCreationDate();
        items.append(out);
    }
    return items;
}

LLViewerInventoryItem* findItem(LLViewerObject* prim, const LLUUID& item_id)
{
    return dynamic_cast<LLViewerInventoryItem*>(prim->getInventoryObject(item_id));
}

LLViewerInventoryItem* findItemNamed(LLViewerObject* prim, const std::string& name, LLAssetType::EType type)
{
    LLInventoryObject::object_list_t contents;
    prim->getInventoryContents(contents);
    for (const LLPointer<LLInventoryObject>& entry : contents)
    {
        LLViewerInventoryItem* item = dynamic_cast<LLViewerInventoryItem*>(entry.get());
        if (item && item->getUUID().notNull() && item->getType() == type && item->getName() == name)
        {
            return item;
        }
    }
    return nullptr;
}

// Waits for the contents of several prims at once.
void fetchContents(const std::vector<LLPointer<LLViewerObject>>& prims, F32 seconds,
                   std::function<void(bool timed_out)> then)
{
    auto waits = std::make_shared<std::vector<std::shared_ptr<ContentsWait>>>();
    for (const LLPointer<LLViewerObject>& prim : prims)
    {
        waits->push_back(std::make_shared<ContentsWait>(prim));
    }
    waitFor(seconds,
            [waits]()
            {
                for (const auto& wait : *waits)
                {
                    if (!wait->mDone)
                    {
                        return false;
                    }
                }
                return true;
            },
            [waits, then](bool late) { then(late); });
}

//-------------------------------------------------------------------------------------------------
// Script and notecard text.

struct AssetAsk
{
    std::function<void(S32 status, const std::string& text, S32 embedded)> then;
};

void onAsset(const LLUUID& asset_id, LLAssetType::EType type, void* user_data, S32 status, LLExtStat)
{
    std::unique_ptr<AssetAsk> ask(static_cast<AssetAsk*>(user_data));
    std::string text;
    S32 embedded = 0;
    if (status == LL_ERR_NOERR)
    {
        LLFileSystem file(asset_id, type);
        S32 size = file.getSize();
        std::string buffer(size > 0 ? size : 0, '\0');
        if (size > 0)
        {
            file.read((U8*)&buffer[0], size);
        }
        if (type == LLAssetType::AT_NOTECARD)
        {
            LLNotecard notecard(LLNotecard::MAX_SIZE);
            std::istringstream stream(buffer);
            if (buffer.empty() || notecard.importStream(stream))
            {
                text = notecard.getText();
                embedded = (S32)notecard.getItems().size();
            }
            else
            {
                status = LL_ERR_ASSET_REQUEST_FAILED;
            }
        }
        else
        {
            // Scripts come with a NUL on the end.
            while (!buffer.empty() && buffer.back() == '\0')
            {
                buffer.pop_back();
            }
            text = buffer;
        }
    }
    ask->then(status, text, embedded);
}

std::string assetError(S32 status)
{
    switch (status)
    {
    case LL_ERR_INSUFFICIENT_PERMISSIONS:
        return "no permission to read it (it needs modify and copy)";
    case LL_ERR_ASSET_REQUEST_NOT_IN_DATABASE:
    case LL_ERR_FILE_EMPTY:
        return "the region has no text for it";
    default:
        return "could not fetch it (status " + std::to_string(status) + ")";
    }
}

void askRunning(LLViewerObject* prim, const LLUUID& item_id)
{
    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_GetScriptRunning);
    msg->nextBlockFast(_PREHASH_Script);
    msg->addUUIDFast(_PREHASH_ObjectID, prim->getID());
    msg->addUUIDFast(_PREHASH_ItemID, item_id);
    msg->sendReliable(prim->getRegion()->getHost());
}

bool canReadWrite(LLViewerInventoryItem* item, std::string& why)
{
    const LLPermissions& perm = item->getPermissions();
    if (!gAgent.allowOperation(PERM_MODIFY, perm, GP_OBJECT_MANIPULATE))
    {
        why = "'" + item->getName() + "' is no-modify";
        return false;
    }
    if (!gAgent.allowOperation(PERM_COPY, perm, GP_OBJECT_MANIPULATE))
    {
        why = "'" + item->getName() + "' is no-copy, and the region only hands out the text of copyable items";
        return false;
    }
    return true;
}

void readItem(const Asker& asker, LLPointer<LLViewerObject> prim, const LLUUID& item_id)
{
    LLViewerInventoryItem* item = findItem(prim, item_id);
    if (!item || !isWanted(item))
    {
        fail(asker, "no script or notecard " + item_id.asString() + " in that prim (list its contents again)");
        return;
    }
    std::string why;
    if (!canReadWrite(item, why))
    {
        fail(asker, why);
        return;
    }
    const bool script = item->getType() == LLAssetType::AT_LSL_TEXT;
    U32 asked_serial = sRunningSerial;
    if (script)
    {
        askRunning(prim, item_id);
    }
    struct Result
    {
        bool have = false;
        F64 at = 0.0;
        S32 status = 0;
        std::string text;
        S32 embedded = 0;
    };
    auto result = std::make_shared<Result>();
    AssetAsk* ask = new AssetAsk;
    ask->then = [result](S32 status, const std::string& text, S32 embedded)
    {
        result->have = true;
        result->at = now();
        result->status = status;
        result->text = text;
        result->embedded = embedded;
    };
    gAssetStorage->getInvItemAsset(prim->getRegion()->getHost(), gAgent.getID(), gAgent.getSessionID(),
                                   item->getPermissions().getOwner(), prim->getID(), item->getUUID(),
                                   item->getAssetUUID(), item->getType(), &onAsset, ask, true);
    const LLUUID prim_id = prim->getID();
    const LLUUID asset = item->getAssetUUID();
    const std::string name = item->getName();
    waitFor(20.f,
            [result, script, prim_id, item_id, asked_serial]()
            {
                if (!result->have)
                {
                    return false;
                }
                if (!script)
                {
                    return true;
                }
                auto running = sRunning.find({ prim_id, item_id });
                return (running != sRunning.end() && running->second.serial > asked_serial) || now() - result->at > 3.0;
            },
            [asker, result, script, prim_id, item_id, asked_serial, asset, name](bool late)
            {
                if (!result->have)
                {
                    fail(asker, "the region did not send '" + name + "' in time");
                    return;
                }
                if (result->status != LL_ERR_NOERR)
                {
                    fail(asker, "'" + name + "': " + assetError(result->status));
                    return;
                }
                LLSD reply;
                reply["text"] = result->text;
                reply["asset"] = asset;
                reply["name"] = name;
                if (script)
                {
                    auto running = sRunning.find({ prim_id, item_id });
                    if (running != sRunning.end() && running->second.serial > asked_serial)
                    {
                        reply["running"] = running->second.running;
                        reply["mono"] = running->second.mono;
                    }
                }
                else
                {
                    reply["embedded"] = result->embedded;
                }
                answer(asker, reply);
            });
}

// Saving a script is compiling it: the answer carries the compiler's errors, if any.
void uploadScript(const Asker& asker, LLPointer<LLViewerObject> prim, const LLUUID& item_id,
                  const std::string& text, bool running, bool mono, const LLUUID& experience)
{
    std::string url = prim->getRegion()->getCapability("UpdateScriptTask");
    if (url.empty())
    {
        fail(asker, "this region cannot save scripts (no UpdateScriptTask)");
        return;
    }
    LLResourceUploadInfo::ptr_t info(std::make_shared<LLScriptAssetUpload>(
        prim->getID(), item_id, mono ? LLScriptAssetUpload::MONO : LLScriptAssetUpload::LSL2, running, experience, text,
        [asker, running, mono](LLUUID item_id, LLUUID task_id, LLUUID new_asset, LLSD response)
        {
            LLSD reply;
            reply["item"] = item_id;
            reply["asset"] = new_asset;
            reply["compiled"] = response["compiled"].asBoolean();
            reply["errors"] = response.has("errors") ? response["errors"] : LLSD::emptyArray();
            reply["running"] = running;
            reply["mono"] = mono;
            answer(asker, reply);
        },
        [asker](LLUUID, LLUUID, LLSD response, std::string reason)
        {
            fail(asker, "the region refused the script: " + reason);
            return true;
        }));
    LLViewerAssetUpload::EnqueueInventoryUpload(url, info);
}

void uploadNotecard(const Asker& asker, LLPointer<LLViewerObject> prim, const LLUUID& item_id, const std::string& text)
{
    std::string url = prim->getRegion()->getCapability("UpdateNotecardTaskInventory");
    if (url.empty())
    {
        fail(asker, "this region cannot save notecards in objects (no UpdateNotecardTaskInventory)");
        return;
    }
    LLNotecard notecard(LLNotecard::MAX_SIZE);
    notecard.setText(text);
    std::ostringstream buffer;
    notecard.exportStream(buffer);
    LLResourceUploadInfo::ptr_t info(std::make_shared<LLBufferedAssetUploadInfo>(
        prim->getID(), item_id, LLAssetType::AT_NOTECARD, buffer.str(),
        [asker](LLUUID item_id, LLUUID task_id, LLUUID new_asset, LLSD)
        {
            LLSD reply;
            reply["item"] = item_id;
            reply["asset"] = new_asset;
            answer(asker, reply);
        },
        [asker](LLUUID, LLUUID, LLSD, std::string reason)
        {
            fail(asker, "the region refused the notecard: " + reason);
            return true;
        }));
    LLViewerAssetUpload::EnqueueInventoryUpload(url, info);
}

// An existing item: for a script, its running state, Mono, and the Experience it was compiled
// with are kept, so saving it does not quietly stop it, turn it to LSL2, or drop its Experience.
void writeExisting(const Asker& asker, LLPointer<LLViewerObject> prim, const LLUUID& item_id, const std::string& text,
                   bool start = false)
{
    LLViewerInventoryItem* item = findItem(prim, item_id);
    if (!item || !isWanted(item))
    {
        fail(asker, "no script or notecard " + item_id.asString() + " in that prim (list its contents again)");
        return;
    }
    std::string why;
    if (!canReadWrite(item, why))
    {
        fail(asker, why);
        return;
    }
    if (item->getType() == LLAssetType::AT_NOTECARD)
    {
        uploadNotecard(asker, prim, item_id, text);
        return;
    }
    struct State
    {
        bool experience_known = false;
        LLUUID experience;
    };
    auto state = std::make_shared<State>();
    std::string url = prim->getRegion()->getCapability("GetMetadata");
    if (url.empty())
    {
        state->experience_known = true;
    }
    else
    {
        LLExperienceCache::instance().fetchAssociatedExperience(prim->getID(), item_id, url,
            [state](const LLSD& experience)
            {
                if (experience.has(LLExperienceCache::EXPERIENCE_ID))
                {
                    state->experience = experience[LLExperienceCache::EXPERIENCE_ID].asUUID();
                    state->experience_known = true;
                }
                else if (experience.has("error") && experience["error"].asInteger() == -1)
                {
                    state->experience_known = true; // asked, and it has none
                }
                // Any other failure leaves it unknown, and the save is refused.
            });
    }
    U32 asked_serial = sRunningSerial;
    askRunning(prim, item_id);
    const LLUUID prim_id = prim->getID();
    waitFor(10.f,
            [state, prim_id, item_id, asked_serial]()
            {
                auto running = sRunning.find({ prim_id, item_id });
                return state->experience_known && running != sRunning.end() && running->second.serial > asked_serial;
            },
            [asker, prim, state, prim_id, item_id, asked_serial, text, start](bool late)
            {
                if (!state->experience_known)
                {
                    // Saved without it, the script would lose its Experience for good.
                    fail(asker, "could not learn whether the script uses an Experience; not saved");
                    return;
                }
                bool running = true;
                bool mono = true;
                auto known = sRunning.find({ prim_id, item_id });
                if (known != sRunning.end() && known->second.serial > asked_serial)
                {
                    running = known->second.running;
                    mono = known->second.mono;
                }
                if (start)
                {
                    running = true; // a new script, made stopped
                }
                if (prim->isDead())
                {
                    fail(asker, "the object went away");
                    return;
                }
                uploadScript(asker, prim, item_id, text, running, mono, state->experience);
            });
}

// Waits until the prim lists an item of that name, then writes the text into it.
void writeOnceListed(const Asker& asker, LLPointer<LLViewerObject> prim, const std::string& name,
                     LLAssetType::EType type, const std::string& text)
{
    auto tries = std::make_shared<int>(0);
    auto contents = std::make_shared<std::shared_ptr<ContentsWait>>();
    auto last = std::make_shared<F64>(0.0);
    waitFor(20.f,
            [prim, name, type, tries, contents, last]()
            {
                if (prim->isDead())
                {
                    return true;
                }
                if (findItemNamed(prim, name, type))
                {
                    return true;
                }
                if (now() - *last > 1.0)
                {
                    *last = now();
                    *contents = std::make_shared<ContentsWait>(prim);
                }
                return false;
            },
            [asker, prim, name, type, text](bool late)
            {
                LLViewerInventoryItem* item = prim->isDead() ? nullptr : findItemNamed(prim, name, type);
                if (!item)
                {
                    fail(asker, "made '" + name + "', but it never showed up in the prim");
                    return;
                }
                writeExisting(asker, prim, item->getUUID(), text, type == LLAssetType::AT_LSL_TEXT);
            });
}

void createScript(const Asker& asker, LLPointer<LLViewerObject> prim, const std::string& name, const std::string& text)
{
    LLPermissions perm;
    perm.init(gAgent.getID(), gAgent.getID(), LLUUID::null, LLUUID::null);
    // Parameters are base, owner, everyone, group, next
    perm.initMasks(PERM_ALL, PERM_ALL, LLFloaterPerms::getEveryonePerms("Scripts"),
                   LLFloaterPerms::getGroupPerms("Scripts"), PERM_MOVE | LLFloaterPerms::getNextOwnerPerms("Scripts"));
    std::string desc;
    LLViewerAssetType::generateDescriptionFor(LLAssetType::AT_LSL_TEXT, desc);
    LLPointer<LLViewerInventoryItem> item = new LLViewerInventoryItem(
        LLUUID::null, LLUUID::null, perm, LLUUID::null, LLAssetType::AT_LSL_TEXT, LLInventoryType::IT_LSL, name, desc,
        LLSaleInfo::DEFAULT, LLInventoryItemFlags::II_FLAGS_NONE, time_corrected());
    // Made stopped: SL's default script would say "Hello, Avatar!" in open chat before the
    // real text arrives. It is started when its text is saved.
    prim->saveScript(item, false, true);
    writeOnceListed(asker, prim, name, LLAssetType::AT_LSL_TEXT, text);
}

// A notecard cannot be made inside an object directly: it is made in your inventory, copied in,
// and the copy left in your inventory goes to the Trash (as the build floater's New Notecard does).
void createNotecard(const Asker& asker, LLPointer<LLViewerObject> prim, const std::string& name, const std::string& text)
{
    const LLUUID folder = gInventory.findCategoryUUIDForType(LLFolderType::FT_NOTECARD);
    LLPointer<LLInventoryCallback> made = new LLBoostFuncInventoryCallback(
        [asker, prim, name, text](const LLUUID& new_id)
        {
            LLViewerInventoryItem* item = gInventory.getItem(new_id);
            if (!item || prim->isDead())
            {
                fail(asker, "could not make the notecard in your inventory");
                return;
            }
            LLToolDragAndDrop::dropInventory(prim, item, LLToolDragAndDrop::SOURCE_AGENT, gAgent.getID());
            change_item_parent(new_id, gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH));
            writeOnceListed(asker, prim, name, LLAssetType::AT_NOTECARD, text);
        });
    create_inventory_item(gAgent.getID(), gAgent.getSessionID(), folder, LLTransactionID::tnull, name, "",
                          LLAssetType::AT_NOTECARD, LLInventoryType::IT_NOTECARD, NO_INV_SUBTYPE,
                          PERM_ALL, made);
}

//-------------------------------------------------------------------------------------------------
// The requests.

void doStatus(const Asker& asker, const LLSD&)
{
    LLSD reply;
    reply["version"] = BRIDGE_VERSION;
    std::string name;
    LLAgentUI::buildFullname(name);
    reply["agent"] = name;
    reply["agent_id"] = gAgent.getID();
    if (LLViewerRegion* region = gAgent.getRegion())
    {
        reply["region"] = region->getName();
        reply["position"] = triple(gAgent.getPositionAgent() - region->getOriginAgent());
    }
    reply["room"] = SpatiandStereo::isStereo();
    if (!sFrames.empty())
    {
        F64 total = 0.0, worst = 0.0;
        for (const auto& frame : sFrames)
        {
            total += frame.second;
            worst = llmax(worst, frame.second);
        }
        reply["fps"] = ll_round(sFrames.size() / total * 10.0) / 10.0;
        reply["worst_frame_ms"] = ll_round(worst * 1000.0);
    }
    answer(asker, reply);
}

void doObjects(const Asker& asker, const LLSD&)
{
    std::vector<LLPointer<LLViewerObject>> roots;
    ownedRoots(roots);
    askNames(roots);
    waitFor(4.f, [roots]() { return askNames(roots); },
            [asker, roots](bool late)
            {
                LLSD list = LLSD::emptyArray();
                for (const LLPointer<LLViewerObject>& root : roots)
                {
                    if (!root->isDead())
                    {
                        list.append(describe(root));
                    }
                }
                LLSD reply;
                reply["objects"] = list;
                if (LLViewerObject* selected = LLSelectMgr::getInstance()->getSelection()->getFirstRootObject(true))
                {
                    reply["selected"] = selected->getRootEdit()->getID();
                }
                answer(asker, reply);
            });
}

void doContents(const Asker& asker, const LLSD& request)
{
    std::string why;
    LLViewerObject* object = ownedObject(request["object"].asUUID(), why);
    if (!object)
    {
        fail(asker, why);
        return;
    }
    LLPointer<LLViewerObject> root = object->getRootEdit();
    std::vector<LLPointer<LLViewerObject>> prims;
    linkset(root, prims);
    fetchContents(prims, 20.f,
        [asker, root, prims](bool late)
        {
            // And the prims' names, which the contents do not carry.
            waitFor(4.f, [prims]() { return askNames(prims); },
                [asker, root, prims, late](bool)
                {
                    if (root->isDead())
                    {
                        fail(asker, "the object went away");
                        return;
                    }
                    LLSD reply;
                    // Not at the top level: "id" there is the request's.
                    reply["object"] = describe(root);
                    LLSD list = LLSD::emptyArray();
                    for (const LLPointer<LLViewerObject>& prim : prims)
                    {
                        if (prim->isDead())
                        {
                            continue;
                        }
                        LLSD out;
                        out["id"] = prim->getID();
                        out["name"] = nameOf(prim);
                        out["root"] = (prim == root);
                        // Where it sits in the linkset: what tells same-named prims apart, and
                        // stays when the linkset is taken and rezzed again.
                        out["local"] = triple(prim == root ? LLVector3::zero : prim->getPosition());
                        out["items"] = itemList(prim);
                        list.append(out);
                    }
                    reply["prim_list"] = list;
                    // A prim without its name would be filed under the wrong folder, so the
                    // answer is incomplete rather than a guess.
                    bool unnamed = false;
                    for (const LLPointer<LLViewerObject>& prim : prims)
                    {
                        if (!prim->isDead() && sNames.find(prim->getID()) == sNames.end()
                            && !(prim == root && !nameOf(prim).empty()))
                        {
                            unnamed = true;
                        }
                    }
                    if (late || unnamed)
                    {
                        reply["incomplete"] = true;
                        reply["why"] = unnamed ? "the region has not told every prim's name yet" : "contents did not all arrive";
                    }
                    answer(asker, reply);
                });
        });
}

LLViewerObject* ownedPrim(const Asker& asker, const LLSD& request)
{
    std::string why;
    LLViewerObject* prim = ownedObject(request["prim"].asUUID(), why);
    if (!prim)
    {
        fail(asker, why);
    }
    return prim;
}

void doRead(const Asker& asker, const LLSD& request)
{
    LLPointer<LLViewerObject> prim = ownedPrim(asker, request);
    if (prim.isNull())
    {
        return;
    }
    const LLUUID item_id = request["item"].asUUID();
    if (request["listed"].asBoolean())
    {
        readItem(asker, prim, item_id);
        return;
    }
    // A listing kept from before can name an asset since replaced, which the asset store would
    // answer from its cache with the old text.
    fetchContents({ prim }, 20.f, [asker, prim, item_id](bool) { readItem(asker, prim, item_id); });
}

void doWrite(const Asker& asker, const LLSD& request)
{
    LLViewerObject* prim = ownedPrim(asker, request);
    if (!prim)
    {
        return;
    }
    const std::string text = request["text"].asString();
    if (request.has("item") && request["item"].asUUID().notNull())
    {
        // "running": true starts it whatever it was: a script stopped by a failed compile is
        // started again by the save that fixes it, as the script editor does.
        writeExisting(asker, prim, request["item"].asUUID(), text, request["running"].asBoolean());
        return;
    }
    const std::string name = request["name"].asString();
    const std::string type = request["type"].asString();
    if (name.empty() || name.size() > (size_t)DB_INV_ITEM_NAME_STR_LEN || name.find('|') != std::string::npos)
    {
        fail(asker, "'" + name + "' cannot be an item name (1 to 63 characters, no '|')");
        return;
    }
    const LLAssetType::EType asset = type == "script" ? LLAssetType::AT_LSL_TEXT : LLAssetType::AT_NOTECARD;
    if (type != "script" && type != "notecard")
    {
        fail(asker, "type is 'script' or 'notecard'");
        return;
    }
    if (findItemNamed(prim, name, asset))
    {
        fail(asker, "the prim already has '" + name + "'; write to its item id");
        return;
    }
    if (asset == LLAssetType::AT_LSL_TEXT)
    {
        createScript(asker, prim, name, text);
    }
    else
    {
        createNotecard(asker, prim, name, text);
    }
}

void doScriptState(const Asker& asker, const LLSD& request, bool reset)
{
    LLViewerObject* prim = ownedPrim(asker, request);
    if (!prim)
    {
        return;
    }
    LLViewerInventoryItem* item = findItem(prim, request["item"].asUUID());
    if (!item || item->getType() != LLAssetType::AT_LSL_TEXT)
    {
        fail(asker, "no such script in that prim");
        return;
    }
    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(reset ? _PREHASH_ScriptReset : _PREHASH_SetScriptRunning);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->nextBlockFast(_PREHASH_Script);
    msg->addUUIDFast(_PREHASH_ObjectID, prim->getID());
    msg->addUUIDFast(_PREHASH_ItemID, item->getUUID());
    if (!reset)
    {
        msg->addBOOLFast(_PREHASH_Running, request["running"].asBoolean());
    }
    msg->sendReliable(prim->getRegion()->getHost());
    answer(asker, LLSD::emptyMap());
}

void doEvents(const Asker& asker, const LLSD& request)
{
    const S64 since = request.has("since") ? request["since"].asInteger() : 0;
    const F32 wait = llclamp((F32)request["wait"].asReal(), 0.f, 60.f);
    auto collect = [since]()
    {
        LLSD list = LLSD::emptyArray();
        for (const LLSD& event : sEvents)
        {
            if (event["seq"].asInteger() > since)
            {
                list.append(event);
            }
        }
        return list;
    };
    waitFor(wait,
            [since]() { return !sEvents.empty() && sEvents.back()["seq"].asInteger() > since; },
            [asker, collect](bool)
            {
                LLSD reply;
                reply["events"] = collect();
                reply["next"] = (LLSD::Integer)(sNextEvent - 1);
                answer(asker, reply);
            });
}

void doSay(const Asker& asker, const LLSD& request)
{
    const S32 channel = request["channel"].asInteger();
    if (channel == 0)
    {
        // Channel 0 is everybody nearby; a tool should not be talking there for you.
        fail(asker, "not on channel 0: that is open chat, heard by everyone nearby");
        return;
    }
    std::string text = request["text"].asString();
    if (text.size() > 1023)
    {
        fail(asker, "too long for one chat line (1023 bytes)");
        return;
    }
    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_ChatFromViewer);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->nextBlockFast(_PREHASH_ChatData);
    msg->addStringFast(_PREHASH_Message, text);
    msg->addU8Fast(_PREHASH_Type, CHAT_TYPE_NORMAL);
    msg->addS32("Channel", channel);
    gAgent.sendReliableMessage();
    answer(asker, LLSD::emptyMap());
}

void doTouch(const Asker& asker, const LLSD& request)
{
    LLViewerObject* prim = ownedPrim(asker, request);
    if (!prim)
    {
        return;
    }
    LLPickInfo pick;
    pick.mObjectFace = request.has("face") ? request["face"].asInteger() : -1;
    send_ObjectGrab_message(prim, pick, LLVector3::zero);
    send_ObjectDeGrab_message(prim, pick);
    answer(asker, LLSD::emptyMap());
}

void doDialogReply(const Asker& asker, const LLSD& request)
{
    std::string why;
    LLViewerObject* object = ownedObject(request["object"].asUUID(), why);
    if (!object)
    {
        fail(asker, why);
        return;
    }
    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_ScriptDialogReply);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->nextBlockFast(_PREHASH_Data);
    msg->addUUIDFast(_PREHASH_ObjectID, object->getID());
    msg->addS32Fast(_PREHASH_ChatChannel, request["channel"].asInteger());
    msg->addS32Fast(_PREHASH_ButtonIndex, request["index"].asInteger());
    msg->addStringFast(_PREHASH_ButtonLabel, request["label"].asString());
    msg->sendReliable(object->getRegion()->getHost());
    answer(asker, LLSD::emptyMap());
}

// For trying things out: a plain box in front of you, named.
void doRez(const Asker& asker, const LLSD& request)
{
    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        fail(asker, "not in a region");
        return;
    }
    LLVector3 at = gAgent.getAtAxis();
    at.mV[VZ] = 0.f;
    at.normVec();
    const LLVector3 place = gAgent.getPositionAgent() + at * 1.5f;
    std::vector<LLPointer<LLViewerObject>> before_list;
    ownedRoots(before_list);
    auto before = std::make_shared<std::set<LLUUID>>();
    for (const LLPointer<LLViewerObject>& object : before_list)
    {
        before->insert(object->getID());
    }

    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_ObjectAdd);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->addUUIDFast(_PREHASH_GroupID, FSCommon::getGroupForRezzing());
    msg->nextBlockFast(_PREHASH_ObjectData);
    msg->addU8Fast(_PREHASH_PCode, LL_PCODE_VOLUME);
    msg->addU8Fast(_PREHASH_Material, LL_MCODE_WOOD);
    msg->addU32Fast(_PREHASH_AddFlags, 0);
    LLVolumeParams volume;
    volume.setType(LL_PCODE_PROFILE_SQUARE, LL_PCODE_PATH_LINE);
    volume.setBeginAndEndS(0.f, 1.f);
    volume.setBeginAndEndT(0.f, 1.f);
    volume.setRatio(1.f, 1.f);
    volume.setShear(0.f, 0.f);
    LLVolumeMessage::packVolumeParams(&volume, msg);
    msg->addVector3Fast(_PREHASH_Scale, LLVector3(0.5f, 0.5f, 0.5f));
    msg->addQuatFast(_PREHASH_Rotation, LLQuaternion());
    msg->addVector3Fast(_PREHASH_RayStart, place);
    msg->addVector3Fast(_PREHASH_RayEnd, place);
    msg->addU8Fast(_PREHASH_BypassRaycast, (U8)1);
    msg->addU8Fast(_PREHASH_RayEndIsIntersection, (U8)0);
    msg->addU8Fast(_PREHASH_State, 0);
    msg->addUUIDFast(_PREHASH_RayTargetID, LLUUID::null);
    msg->sendReliable(region->getHost());

    auto made = std::make_shared<LLPointer<LLViewerObject>>();
    const std::string name = request["name"].asString();
    waitFor(10.f,
            [before, made, place]()
            {
                std::vector<LLPointer<LLViewerObject>> roots;
                ownedRoots(roots);
                for (const LLPointer<LLViewerObject>& object : roots)
                {
                    if (!before->count(object->getID()) && !object->isAttachment()
                        && dist_vec(object->getPositionAgent(), place) < 2.f)
                    {
                        *made = object;
                        return true;
                    }
                }
                return false;
            },
            [asker, made, name](bool late)
            {
                if (made->isNull())
                {
                    fail(asker, "nothing appeared: building may not be allowed here");
                    return;
                }
                LLViewerObject* object = *made;
                if (!name.empty())
                {
                    LLMessageSystem* msg = gMessageSystem;
                    msg->newMessageFast(_PREHASH_ObjectName);
                    msg->nextBlockFast(_PREHASH_AgentData);
                    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
                    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
                    msg->nextBlockFast(_PREHASH_ObjectData);
                    msg->addU32Fast(_PREHASH_LocalID, object->getLocalID());
                    msg->addStringFast(_PREHASH_Name, name);
                    msg->sendReliable(object->getRegion()->getHost());
                    sNames[object->getID()] = { name, now() };
                }
                LLSD reply;
                reply["object"] = object->getID();
                answer(asker, reply);
            });
}

S32 attachmentPoint(const LLSD& point)
{
    if (!isAgentAvatarValid())
    {
        return -1;
    }
    if (point.isInteger())
    {
        return point.asInteger();
    }
    std::string wanted = point.asString();
    LLStringUtil::toLower(wanted);
    for (const auto& entry : gAgentAvatarp->mAttachmentPoints)
    {
        std::string name = entry.second ? entry.second->getName() : std::string();
        LLStringUtil::toLower(name);
        if (name == wanted)
        {
            return entry.first;
        }
    }
    return -1;
}

void doAttach(const Asker& asker, const LLSD& request)
{
    std::string why;
    LLViewerObject* object = ownedObject(request["object"].asUUID(), why);
    if (!object)
    {
        fail(asker, why);
        return;
    }
    object = object->getRootEdit();
    if (object->isAttachment())
    {
        fail(asker, "already worn");
        return;
    }
    const S32 point = attachmentPoint(request["point"]);
    if (point <= 0)
    {
        fail(asker, "no attachment point '" + request["point"].asString() + "' (e.g. \"HUD Center\")");
        return;
    }
    // What is already worn there. Worn from the ground an object may keep its id or be given a
    // new one, so the wait is for whatever is newly worn at that point.
    std::vector<LLPointer<LLViewerObject>> before_list;
    ownedRoots(before_list);
    auto before = std::make_shared<std::set<LLUUID>>();
    for (const LLPointer<LLViewerObject>& o : before_list)
    {
        if (o->isAttachment())
        {
            before->insert(o->getID());
        }
    }
    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_ObjectAttach);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->addU8Fast(_PREHASH_AttachmentPoint, (U8)point | ATTACHMENT_ADD);
    msg->nextBlockFast(_PREHASH_ObjectData);
    msg->addU32Fast(_PREHASH_ObjectLocalID, object->getLocalID());
    msg->addQuatFast(_PREHASH_Rotation, LLQuaternion());
    msg->sendReliable(object->getRegion()->getHost());

    // Worn, it comes back as a new object.
    auto worn = std::make_shared<LLPointer<LLViewerObject>>();
    waitFor(15.f,
            [before, worn, point]()
            {
                std::vector<LLPointer<LLViewerObject>> roots;
                ownedRoots(roots);
                for (const LLPointer<LLViewerObject>& o : roots)
                {
                    S32 at = -1;
                    if (!before->count(o->getID()) && o->isAttachment())
                    {
                        attachmentPointName(o, &at);
                        if (at == point)
                        {
                            *worn = o;
                            return true;
                        }
                    }
                }
                return false;
            },
            [asker, worn](bool)
            {
                if (worn->isNull())
                {
                    fail(asker, "it was not worn in time");
                    return;
                }
                LLSD reply;
                reply["object"] = (*worn)->getID();
                answer(asker, reply);
            });
}

void doRemove(const Asker& asker, const LLSD& request, bool take)
{
    std::string why;
    LLViewerObject* object = ownedObject(request["object"].asUUID(), why);
    if (!object)
    {
        fail(asker, why);
        return;
    }
    object = object->getRootEdit();
    LLMessageSystem* msg = gMessageSystem;
    if (object->isAttachment())
    {
        // Taking off a worn object puts it back in your inventory.
        msg->newMessageFast(_PREHASH_ObjectDetach);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_ObjectLocalID, object->getLocalID());
        msg->sendReliable(object->getRegion()->getHost());
    }
    else if (take)
    {
        LLTransactionID tid;
        tid.generate();
        msg->newMessageFast(_PREHASH_DeRezObject);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_AgentBlock);
        msg->addUUIDFast(_PREHASH_GroupID, gAgent.getGroupID());
        msg->addU8Fast(_PREHASH_Destination, (U8)DRD_TAKE_INTO_AGENT_INVENTORY);
        msg->addUUIDFast(_PREHASH_DestinationID, gInventory.findCategoryUUIDForType(LLFolderType::FT_OBJECT));
        msg->addUUIDFast(_PREHASH_TransactionID, tid);
        msg->addU8Fast(_PREHASH_PacketCount, 1);
        msg->addU8Fast(_PREHASH_PacketNumber, 0);
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_ObjectLocalID, object->getLocalID());
        msg->sendReliable(object->getRegion()->getHost());
    }
    else
    {
        fail(asker, "not worn");
        return;
    }
    const LLUUID id = object->getID();
    waitFor(15.f,
            [id]()
            {
                LLViewerObject* o = gObjectList.findObject(id);
                return !o || o->isDead();
            },
            [asker](bool late)
            {
                if (late)
                {
                    fail(asker, "it is still there");
                    return;
                }
                answer(asker, LLSD::emptyMap());
            });
}

// For seeing what the stereo UI layers hold: the next frame's, as PNG files on this machine.
void doDumpUI(const Asker& asker, const LLSD& request)
{
    if (!SpatiandStereo::isStereo())
    {
        fail(asker, "only in the room: a window has no layers");
        return;
    }
    std::string dir = request.has("dir") ? request["dir"].asString() : std::string("/tmp/spatiworld-layers");
    LLFile::mkdir(dir);
    SpatiandStereo::dumpLayers(dir);
    waitFor(5.f, []() { return !SpatiandStereo::dumpingLayers(); },
            [asker, dir](bool late)
            {
                if (late)
                {
                    fail(asker, "no frame drew the layers");
                    return;
                }
                LLSD reply;
                reply["dir"] = dir;
                answer(asker, reply);
            });
}

// For trying out linksets: link your objects, the first one the root.
void doLink(const Asker& asker, const LLSD& request)
{
    std::vector<LLPointer<LLViewerObject>> objects;
    for (const LLSD& id : llsd::inArray(request["objects"]))
    {
        std::string why;
        LLViewerObject* object = ownedObject(id.asUUID(), why);
        if (!object)
        {
            fail(asker, why);
            return;
        }
        if (object->isAttachment())
        {
            fail(asker, "worn objects cannot be linked");
            return;
        }
        objects.push_back(object->getRootEdit());
    }
    if (objects.size() < 2)
    {
        fail(asker, "give at least two objects, the root first");
        return;
    }
    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_ObjectLink);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    for (const LLPointer<LLViewerObject>& object : objects)
    {
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_ObjectLocalID, object->getLocalID());
    }
    msg->sendReliable(objects[0]->getRegion()->getHost());
    const LLUUID root = objects[0]->getID();
    const size_t count = objects.size();
    waitFor(10.f,
            [root, count]()
            {
                LLViewerObject* o = gObjectList.findObject(root);
                if (!o || o->isDead())
                {
                    return false;
                }
                std::vector<LLPointer<LLViewerObject>> prims;
                linkset(o, prims);
                return prims.size() >= count;
            },
            [asker, root](bool late)
            {
                if (late)
                {
                    fail(asker, "they were not linked (too far apart, or not modifiable?)");
                    return;
                }
                LLSD reply;
                reply["object"] = root;
                answer(asker, reply);
            });
}

// And naming a prim, which a new box or a child of a linkset needs.
void doName(const Asker& asker, const LLSD& request)
{
    LLViewerObject* prim = ownedPrim(asker, request);
    if (!prim)
    {
        return;
    }
    const std::string name = request["name"].asString();
    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_ObjectName);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->nextBlockFast(_PREHASH_ObjectData);
    msg->addU32Fast(_PREHASH_LocalID, prim->getLocalID());
    msg->addStringFast(_PREHASH_Name, name);
    msg->sendReliable(prim->getRegion()->getHost());
    sNames[prim->getID()] = { name, now() };
    answer(asker, LLSD::emptyMap());
}

// For finding out what a click at a place on the UI reaches: the object the viewer's own hit test
// finds there, HUD or world. Raw canvas pixels from the bottom left, as in a dump_ui picture.
void doPick(const Asker& asker, const LLSD& request)
{
    const LLVector2 scale = gViewerWindow->getDisplayScale();
    const S32 x = ll_round((F32)request["x"].asReal() / scale.mV[VX]);
    const S32 y = ll_round((F32)request["y"].asReal() / scale.mV[VY]);
    S32 face = -1;
    LLViewerObject* hit = gViewerWindow->cursorIntersect(x, y, 512.f, nullptr, -1, true, false, true, false, &face);
    LLSD reply;
    LLSD ui = LLSD::emptyArray();
    ui.append(x);
    ui.append(y);
    reply["ui"] = ui;
    reply["hud_point"] = triple(gViewerWindow->mousePointHUD(x, y));
    LLRect box;
    if (SpatiandStereo::hudBox(box))
    {
        reply["hud_box"] = llsd::array(box.mLeft, box.mBottom, box.mRight, box.mTop);
    }
    if (hit)
    {
        LLViewerObject* root = hit->getRootEdit();
        reply["object"] = root->getID();
        reply["prim"] = hit->getID();
        reply["name"] = nameOf(root);
        reply["hud"] = hit->isHUDAttachment();
        reply["face"] = face;
    }
    answer(asker, reply);
}

// And clicking there, through the viewer's own mouse handling: what a click from spatiand does
// once the window has turned it into canvas coordinates. Raw canvas pixels.
void doUIClick(const Asker& asker, const LLSD& request)
{
    const LLCoordGL pos(ll_round((F32)request["x"].asReal()), ll_round((F32)request["y"].asReal()));
    LLWindow* window = gViewerWindow->getWindow();
    gViewerWindow->handleMouseMove(window, pos, MASK_NONE);
    gViewerWindow->handleMouseDown(window, pos, MASK_NONE);
    // Let go a moment later, as a hand would.
    waitFor(0.15f, []() { return false; },
            [asker, pos](bool)
            {
                gViewerWindow->handleMouseUp(gViewerWindow->getWindow(), pos, MASK_NONE);
                // What the tool made of it.
                LLSD reply;
                if (LLTool* tool = LLToolMgr::getInstance()->getCurrentTool())
                {
                    reply["tool"] = tool->getName();
                }
                if (LLViewerObject* picked = LLToolPie::getInstance()->getPick().getObject())
                {
                    reply["picked"] = picked->getID();
                    reply["picked_hud"] = picked->isHUDAttachment();
                }
                answer(asker, reply);
            });
}

// Laying out prims of your own objects, a HUD's buttons say: where a prim sits (relative to its
// root, or to the attachment point for a worn root) and its size, as the edit tool sets them.
void doPlace(const Asker& asker, const LLSD& request)
{
    LLViewerObject* prim = ownedPrim(asker, request);
    if (!prim)
    {
        return;
    }
    auto vec = [](const LLSD& v) { return LLVector3((F32)v[0].asReal(), (F32)v[1].asReal(), (F32)v[2].asReal()); };
    U8 type = 0;
    U8 data[64];
    S32 offset = 0;
    if (request.has("position"))
    {
        const LLVector3 position = vec(request["position"]);
        type |= UPD_POSITION;
        htolememcpy(&data[offset], position.mV, MVT_LLVector3, 12);
        offset += 12;
    }
    if (request.has("scale"))
    {
        const LLVector3 scale = vec(request["scale"]);
        type |= UPD_SCALE;
        htolememcpy(&data[offset], scale.mV, MVT_LLVector3, 12);
        offset += 12;
    }
    if (!type)
    {
        fail(asker, "give a position, a scale, or both");
        return;
    }
    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_MultipleObjectUpdate);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->nextBlockFast(_PREHASH_ObjectData);
    msg->addU32Fast(_PREHASH_ObjectLocalID, prim->getLocalID());
    msg->addU8Fast(_PREHASH_Type, type);
    msg->addBinaryDataFast(_PREHASH_Data, data, offset);
    msg->sendReliable(prim->getRegion()->getHost());
    answer(asker, LLSD::emptyMap());
}

void doRoom(const Asker& asker, const LLSD& request)
{
    // For comparing what the room draws with what a window draws, in the same session.
    SpatiandStereo::setRoom(request["on"].asBoolean());
    answer(asker, LLSD::emptyMap());
}

void dispatch(U32 client, const std::string& line)
{
    Asker asker;
    asker.client = client;
    boost::system::error_code error;
    boost::json::value parsed = boost::json::parse(line, error);
    if (error || !parsed.is_object())
    {
        fail(asker, "not a JSON object: " + line.substr(0, 80));
        return;
    }
    LLSD request = LlsdFromJson(parsed);
    asker.id = request["id"];
    const std::string command = request["cmd"].asString();
    if (LLStartUp::getStartupState() < STATE_STARTED || !gAgent.getRegion() || !isAgentAvatarValid())
    {
        fail(asker, "not logged in yet");
        return;
    }
    static const std::map<std::string, std::function<void(const Asker&, const LLSD&)>> commands = {
        { "status", doStatus },
        { "objects", doObjects },
        { "contents", doContents },
        { "read", doRead },
        { "write", doWrite },
        { "reset", [](const Asker& a, const LLSD& r) { doScriptState(a, r, true); } },
        { "set_running", [](const Asker& a, const LLSD& r) { doScriptState(a, r, false); } },
        { "events", doEvents },
        { "say", doSay },
        { "touch", doTouch },
        { "dialog_reply", doDialogReply },
        { "rez", doRez },
        { "attach", doAttach },
        { "detach", [](const Asker& a, const LLSD& r) { doRemove(a, r, false); } },
        { "take", [](const Asker& a, const LLSD& r) { doRemove(a, r, true); } },
        { "room", doRoom },
        { "place", doPlace },
        { "pick", doPick },
        { "ui_click", doUIClick },
        { "link", doLink },
        { "name", doName },
        { "dump_ui", doDumpUI },
    };
    auto found = commands.find(command);
    if (found == commands.end())
    {
        fail(asker, "no command '" + command + "'");
        return;
    }
    found->second(asker, request);
}

//-------------------------------------------------------------------------------------------------
// The socket.

#if SPATIWORLD_BRIDGE_SOCKET

std::string socketPath()
{
    std::string dir;
    const char* runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && runtime[0])
    {
        dir = std::string(runtime) + "/spatiworld";
    }
    else
    {
        dir = gDirUtilp->getOSUserAppDir() + "/run";
    }
    mkdir(dir.c_str(), 0700);
    chmod(dir.c_str(), 0700);
    return dir + "/bridge.sock";
}

void nonBlocking(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

bool openSocket()
{
    const std::string path = socketPath();
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path))
    {
        LL_WARNS("SpatiWorldBridge") << "socket path too long: " << path << LL_ENDL;
        return false;
    }
    strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    // Another SpatiWorld already answering there: leave it be.
    int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe >= 0)
    {
        bool taken = connect(probe, (sockaddr*)&address, sizeof(address)) == 0;
        close(probe);
        if (taken)
        {
            LL_WARNS("SpatiWorldBridge") << "another viewer already has " << path << LL_ENDL;
            return false;
        }
    }
    unlink(path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return false;
    }
    mode_t mask = umask(0077);
    bool bound = bind(fd, (sockaddr*)&address, sizeof(address)) == 0;
    umask(mask);
    if (!bound || listen(fd, 4) != 0)
    {
        LL_WARNS("SpatiWorldBridge") << "cannot listen on " << path << ": " << strerror(errno) << LL_ENDL;
        close(fd);
        return false;
    }
    chmod(path.c_str(), 0600);
    nonBlocking(fd);
    sListenFd = fd;
    LL_INFOS("SpatiWorldBridge") << "listening on " << path << LL_ENDL;
    return true;
}

void closeSocket()
{
    for (auto& client : sClients)
    {
        close(client.second.fd);
    }
    sClients.clear();
    if (sListenFd >= 0)
    {
        close(sListenFd);
        sListenFd = -1;
        unlink(socketPath().c_str());
        LL_INFOS("SpatiWorldBridge") << "closed" << LL_ENDL;
    }
}

void pollSocket()
{
    for (;;)
    {
        int fd = accept(sListenFd, nullptr, nullptr);
        if (fd < 0)
        {
            break;
        }
        nonBlocking(fd);
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        sClients[sNextClient++].fd = fd;
    }
    std::vector<U32> gone;
    std::vector<std::pair<U32, std::string>> lines;
    for (auto& entry : sClients)
    {
        Client& client = entry.second;
        char buffer[65536];
        for (;;)
        {
            ssize_t n = recv(client.fd, buffer, sizeof(buffer), 0);
            if (n > 0)
            {
                client.in.append(buffer, n);
                continue;
            }
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            {
                gone.push_back(entry.first);
            }
            break;
        }
        size_t start = 0, end;
        while ((end = client.in.find('\n', start)) != std::string::npos)
        {
            if (end > start)
            {
                lines.emplace_back(entry.first, client.in.substr(start, end - start));
            }
            start = end + 1;
        }
        client.in.erase(0, start);
        if (client.in.size() > MAX_LINE)
        {
            gone.push_back(entry.first);
        }
    }
    for (const auto& line : lines)
    {
        dispatch(line.first, line.second);
    }
    runWaits();
    for (auto& entry : sClients)
    {
        Client& client = entry.second;
        while (!client.out.empty())
        {
#ifdef MSG_NOSIGNAL
            ssize_t n = send(client.fd, client.out.data(), client.out.size(), MSG_NOSIGNAL);
#else
            ssize_t n = send(client.fd, client.out.data(), client.out.size(), 0);
#endif
            if (n > 0)
            {
                client.out.erase(0, n);
                continue;
            }
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            {
                gone.push_back(entry.first);
            }
            break;
        }
    }
    for (U32 id : gone)
    {
        auto it = sClients.find(id);
        if (it != sClients.end())
        {
            close(it->second.fd);
            sClients.erase(it);
        }
    }
}

#endif // SPATIWORLD_BRIDGE_SOCKET

void idle(void*)
{
#if SPATIWORLD_BRIDGE_SOCKET
    static LLCachedControl<bool> enabled(gSavedSettings, "SpatiWorldBridge", false);
    if (enabled && sListenFd < 0 && now() >= sNextOpenTry)
    {
        if (!openSocket())
        {
            sNextOpenTry = now() + 10.0;
        }
    }
    else if (!enabled && sListenFd >= 0)
    {
        closeSocket();
        sWaits.clear();
    }
    if (sListenFd >= 0)
    {
        noteFrame();
        pollSocket();
    }
#endif
}

} // namespace

//-------------------------------------------------------------------------------------------------

void SpatiWorldBridge::start()
{
    static bool started = false;
    if (!started)
    {
        started = true;
        gIdleCallbacks.addFunction(&idle, nullptr);
    }
}

void SpatiWorldBridge::heardChat(const LLUUID& from_id, const LLUUID& owner_id, U8 source_type, U8 chat_type,
                                 const std::string& from_name, const std::string& text)
{
    if (sListenFd < 0 || source_type != CHAT_SOURCE_OBJECT || owner_id != gAgent.getID())
    {
        return;
    }
    if (chat_type == CHAT_TYPE_START || chat_type == CHAT_TYPE_STOP)
    {
        return;
    }
    static const std::map<U8, const char*> kinds = {
        { CHAT_TYPE_WHISPER, "whisper" }, { CHAT_TYPE_NORMAL, "say" },   { CHAT_TYPE_SHOUT, "shout" },
        { CHAT_TYPE_DEBUG_MSG, "debug" }, { CHAT_TYPE_REGION, "region" }, { CHAT_TYPE_OWNER, "owner" },
        { CHAT_TYPE_DIRECT, "direct" },
    };
    auto kind = kinds.find(chat_type);
    LLSD event;
    event["kind"] = kind != kinds.end() ? kind->second : "chat";
    event["object"] = from_id;
    event["name"] = from_name;
    event["text"] = text;
    keepEvent(event);
}

void SpatiWorldBridge::heardDialog(const LLUUID& object_id, const LLUUID& owner_id, const std::string& object_name,
                                   const std::string& message, S32 channel, const LLSD& buttons)
{
    if (sListenFd < 0 || owner_id != gAgent.getID())
    {
        return;
    }
    LLSD event;
    event["kind"] = "dialog";
    event["object"] = object_id;
    event["name"] = object_name;
    event["text"] = message;
    event["channel"] = channel;
    event["buttons"] = buttons;
    keepEvent(event);
}

void SpatiWorldBridge::heardObjectName(const LLUUID& id, const std::string& name)
{
    sNames[id] = { name, now() };
}

void SpatiWorldBridge::heardScriptRunning(const LLUUID& object_id, const LLUUID& item_id, bool running, bool mono)
{
    Running& state = sRunning[{ object_id, item_id }];
    state.running = running;
    state.mono = mono;
    state.serial = ++sRunningSerial;
}
