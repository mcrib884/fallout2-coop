// Co-op synchronized loot/steal.
//
// The vanilla loot window cannot run on the host for a remote player: the
// window is modal UI on the clicker's machine, and the host-side looting
// path (`inventoryOpenLooting` with a non-local looter) is a silent no-op by
// design. This module relays the whole interaction:
//
//   host  --NET_PKT_LOOT_STATE(OPEN, fid/pid/items)-->  client
//   client --NET_PKT_LOOT_CMD(MOVE pid qty)----------->  host
//   host applies on the real objects (steal roll for steal sessions),
//   echoes --NET_PKT_LOOT_STATE(items + dude delta + msg)--> client
//   client replays the delta onto its own inventory, rebuilds the mirror
//
// The client runs the real vanilla loot/steal window against a local mirror
// object (same fid/pid as the target, inventory from the host), so all
// vanilla UI behavior (drag, quantity select, party slots) keeps working.

#include "multiplayer_loot.h"

#include <algorithm>
#include <cstring>

#include "animation.h"
#include "art.h"
#include "combat.h"
#include "critter.h"
#include "debug.h"
#include "display_monitor.h"
#include "game.h"
#include "inventory.h"
#include "item.h"
#include "multiplayer.h"
#include "multiplayer_profile.h"
#include "net.h"
#include "object.h"
#include "party_member.h"
#include "perk.h"
#include "proto.h"
#include "scripts.h"
#include "skill.h"
#include "stat.h"

namespace fallout {

// ---------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------

struct MpLootHostSession {
    bool active;
    uint8_t netId;
    bool isSteal;
    uint32_t targetNetId;
    bool wasCaught;
    int stealCount;   // per-session repeated-steal counter (feeds the roll modifier)
    int stealingXp;   // accumulated successful steal XP, awarded at END
    int stealXpBonus;
};

static MpLootHostSession gLootHost[NET_MAX_PLAYERS];

static Object* mpLootFindItemByPid(Object* container, uint32_t pid)
{
    if (container == nullptr) {
        return nullptr;
    }
    Inventory* inventory = &container->data.inventory;
    for (int index = 0; index < inventory->length; index++) {
        if (inventory->items[index].item->pid == pid) {
            return inventory->items[index].item;
        }
    }
    return nullptr;
}

static int mpLootCollectItems(Object* container, NetLootItem* out, int maxOut)
{
    if (container == nullptr || maxOut <= 0) {
        return 0;
    }
    Inventory* inventory = &container->data.inventory;
    int count = 0;
    for (int index = 0; index < inventory->length && count < maxOut; index++) {
        out[count].pid = inventory->items[index].item->pid;
        out[count].qty = inventory->items[index].quantity;
        count++;
    }
    return count;
}

// Sends the authoritative state for a session: the target's item snapshot,
// the deltas applied to the looter's own inventory, and a feedback message.
static void mpLootHostSendState(MpLootHostSession* s, uint8_t op, bool ok,
    const char* msg, const NetLootItem* deltas, int deltaCount)
{
    Object* target = MpFindObjByNetId(s->targetNetId);
    NetLootItem items[NET_LOOT_MAX_ITEMS];
    int itemCount = 0;
    if (op != NET_LOOT_OP_END && target != nullptr) {
        itemCount = mpLootCollectItems(target, items, NET_LOOT_MAX_ITEMS);
    }
    deltaCount = std::min(deltaCount, NET_LOOT_MAX_ITEMS);

    char buffer[sizeof(NetLootStatePayload) + 2 * NET_LOOT_MAX_ITEMS * sizeof(NetLootItem)];
    NetLootStatePayload* p = reinterpret_cast<NetLootStatePayload*>(buffer);
    memset(p, 0, sizeof(*p));
    p->op = op;
    p->netId = s->netId;
    p->isSteal = s->isSteal ? 1 : 0;
    p->lastOk = ok ? 1 : 0;
    p->targetItemCount = static_cast<uint8_t>(itemCount);
    p->dudeDeltaCount = static_cast<uint8_t>(deltaCount);
    p->targetNetId = s->targetNetId;
    p->targetFid = target != nullptr ? target->fid : 0;
    p->targetPid = target != nullptr ? target->pid : 0;
    if (msg != nullptr && msg[0] != '\0') {
        strncpy(p->msgText, msg, sizeof(p->msgText) - 1);
    }

    uint8_t* cursor = reinterpret_cast<uint8_t*>(p + 1);
    memcpy(cursor, items, itemCount * sizeof(NetLootItem));
    cursor += itemCount * sizeof(NetLootItem);
    memcpy(cursor, deltas, deltaCount * sizeof(NetLootItem));

    size_t total = sizeof(NetLootStatePayload)
        + (itemCount + deltaCount) * sizeof(NetLootItem);

    const MultiplayerPlayer* player = &gMpSession.players[s->netId - 1];
    if (player->peer != nullptr) {
        NetSendPacket(player->peer, NET_CHANNEL_RELIABLE, NET_PKT_LOOT_STATE, buffer, total);
        debugFilePrint("MPLOOT host state netId=%u op=%d ok=%d items=%d deltas=%d",
            s->netId, op, ok, itemCount, deltaCount);
    }
}

// Sends the OPEN state once the avatar is adjacent to the target.
static void mpLootHostOpen(uint8_t netId)
{
    MpLootHostSession* s = &gLootHost[netId - 1];
    if (!s->active) {
        return;
    }
    Object* target = MpFindObjByNetId(s->targetNetId);
    if (target == nullptr) {
        s->active = false;
        return;
    }
    mpLootHostSendState(s, NET_LOOT_OP_OPEN, true, nullptr, nullptr, 0);
}

// Walk-arrival callback: the animation system passes the registered
// (avatar, target) pair; the session is found through the avatar.
static int mpLootHostArrived(void* a1, void* a2)
{
    Object* avatar = static_cast<Object*>(a1);
    if (avatar == nullptr) {
        return 0;
    }
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        MpLootHostSession* s = &gLootHost[i];
        if (s->active && gMpSession.players[i].isConnected
            && gMpSession.players[i].obj == avatar) {
            mpLootHostOpen(s->netId);
            break;
        }
    }
    return 0;
}

// Awards the vanilla close-time steal XP on the avatar's sheet (mirrors
// MpDialogGrantExperience's per-avatar math). overrideMsg, when non-null,
// replaces the XP message (used for the caught-steal close). The session is
// idempotent: a second END (retransmit, double close) is a no-op.
static void mpLootHostEnd(uint8_t netId, const char* overrideMsg)
{
    MpLootHostSession* s = &gLootHost[netId - 1];
    if (!s->active) {
        return;
    }
    s->active = false;

    Object* target = MpFindObjByNetId(s->targetNetId);
    Object* avatar = gMpSession.players[netId - 1].obj;

    char msg[64] = { 0 };
    if (overrideMsg != nullptr) {
        strncpy(msg, overrideMsg, sizeof(msg) - 1);
    } else if (s->isSteal && target != nullptr && avatar != nullptr
        && !s->wasCaught && s->stealingXp > 0 && !objectIsPartyMember(target)) {
        int capped = std::min(300 - skillGetValue(avatar, SKILL_STEAL), s->stealingXp);
        if (capped > 0) {
            int currentXp = MpProfileGetPcStat(avatar, PC_STAT_EXPERIENCE);
            int swiftLearner = perkGetRank(avatar, PERK_SWIFT_LEARNER);
            int newXp = currentXp + capped + swiftLearner * 5 * capped / 100;
            int level = MpProfileGetPcStat(avatar, PC_STAT_LEVEL);
            int maxHpBefore = critterGetStat(avatar, STAT_MAXIMUM_HIT_POINTS);
            while (level < 99) {
                int nextXp = pcGetExperienceForLevel(level + 1);
                if (nextXp == -1 || newXp < nextXp) {
                    break;
                }
                level++;
                int endurance = critterGetBaseStatWithTraitModifier(avatar, STAT_ENDURANCE);
                int hpPerLevel = endurance / 2 + 2 + perkGetRank(avatar, PERK_LIFEGIVER) * 4;
                critterSetBonusStat(avatar, STAT_MAXIMUM_HIT_POINTS,
                    critterGetBonusStat(avatar, STAT_MAXIMUM_HIT_POINTS) + hpPerLevel);
                critterAdjustHitPoints(avatar,
                    critterGetStat(avatar, STAT_MAXIMUM_HIT_POINTS) - maxHpBefore);
                maxHpBefore = critterGetStat(avatar, STAT_MAXIMUM_HIT_POINTS);
            }
            MpProfileSetPcStat(avatar, PC_STAT_EXPERIENCE, newXp);
            MpProfileSetPcStat(avatar, PC_STAT_LEVEL, level);
            inventoryFormatMessage(29, newXp - currentXp, msg, sizeof(msg));
            debugFilePrint("MPLOOT host steal xp netId=%u capped=%d level=%d", netId, capped, level);
        }
    }

    mpLootHostSendState(s, NET_LOOT_OP_END, true, msg[0] != '\0' ? msg : nullptr, nullptr, 0);
    debugFilePrint("MPLOOT host end netId=%u", netId);
}

// Refreshes every other open session targeting the same object (another
// player's move changed the shared inventory; their mirrors must not stay
// stale). The refresh reuses the MOVE op with an empty delta list — the
// client rebuilds its mirror from the fresh snapshot alone.
static void mpLootHostRefreshOtherSessions(uint8_t actingNetId, uint32_t targetNetId)
{
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        if (i + 1 == actingNetId) {
            continue;
        }
        MpLootHostSession* s = &gLootHost[i];
        if (s->active && s->targetNetId == targetNetId) {
            mpLootHostSendState(s, NET_LOOT_OP_MOVE, true, nullptr, nullptr, 0);
        }
    }
}

static void mpLootHostMove(uint8_t netId, uint32_t pid, int32_t qty)
{
    MpLootHostSession* s = &gLootHost[netId - 1];
    if (!s->active || qty == 0) {
        return;
    }
    Object* target = MpFindObjByNetId(s->targetNetId);
    if (target == nullptr) {
        s->active = false;
        return;
    }
    Object* avatar = gMpSession.players[netId - 1].obj;
    if (avatar == nullptr) {
        return;
    }
    // The world keeps running while the client's modal window is open; the
    // target may have walked out of reach. Vanilla can never hit this.
    if (!objectWithinWalkDistance(avatar, target)) {
        mpLootHostSendState(s, NET_LOOT_OP_MOVE, false, nullptr, nullptr, 0);
        return;
    }

    bool plant = qty < 0;
    int amount = qty < 0 ? -qty : qty;
    Object* from = plant ? avatar : target;
    Object* to = plant ? target : avatar;
    Object* item = mpLootFindItemByPid(from, pid);
    if (item == nullptr) {
        mpLootHostSendState(s, NET_LOOT_OP_MOVE, false, nullptr, nullptr, 0);
        return;
    }

    char msg[64] = { 0 };
    NetLootItem deltas[1];
    int deltaCount = 0;
    bool ok = false;

    // Steal sessions roll only against active critter targets — the same
    // rule the vanilla window uses to decide _gIsSteal (stealing from a
    // container is plain looting).
    bool doRoll = s->isSteal
        && PID_TYPE(target->pid) == OBJ_TYPE_CRITTER
        && critterIsActive(target);
    if (doRoll) {
        s->stealCount++;
        int savedStealCount = _gStealCount;
        _gStealCount = s->stealCount; // feeds the repeated-steal modifier
        int xpOverride = -1;
        skillSetRemoteMessageSink(msg, sizeof(msg));
        SkillStealResult stealResult = skillsPerformStealing(avatar, target, item, amount, plant, &xpOverride);
        skillSetRemoteMessageSink(nullptr, 0);
        _gStealCount = savedStealCount;
        if (stealResult == SkillStealResult::Success) {
            if (itemMove(from, to, item, amount) == 0) {
                ok = true;
                deltaCount = 1;
                deltas[0].pid = pid;
                deltas[0].qty = plant ? -amount : amount;
                s->stealingXp += (xpOverride >= 0 ? xpOverride : s->stealXpBonus);
                s->stealXpBonus += 10;
            }
        } else if (stealResult == SkillStealResult::Caught) {
            // Vanilla parity: the window closes on a caught steal and the
            // world reacts immediately — run the target's PICKUP script now
            // (not deferred to window close) and close the session with the
            // caught message (no XP). Further moves are rejected.
            s->wasCaught = true;
            int sid = -1;
            if (objectGetSid(target, &sid) != -1) {
                // Diagnostics: prove whether the reaction script exists,
                // whether it is alive, and whether it changed the target's
                // team (hostility display) — the vanilla reaction is 100%
                // script-driven and can fail silently.
                Script* script = nullptr;
                const char* scriptName = "?";
                int progFlags = 0;
                int hasPickupProc = -1;
                if (scriptGetScript(sid, &script) != -1 && script->program != nullptr) {
                    scriptName = script->program->name != nullptr ? script->program->name : "?";
                    progFlags = script->program->flags;
                    hasPickupProc = programFindProcedure(script->program, "pickup_p_proc");
                }
                int teamBefore = target->data.critter.combat.team;
                scriptSetObjects(sid, avatar, nullptr);
                scriptExecProc(sid, SCRIPT_PROC_PICKUP);
                debugFilePrint("MPLOOT host caught pickup sid=%d script='%s' progFlags=0x%X pickupProc=%d team=%d->%d",
                    sid, scriptName, progFlags, hasPickupProc,
                    teamBefore, target->data.critter.combat.team);
            }
            mpLootHostEnd(netId, msg[0] != '\0' ? msg : nullptr);
            debugFilePrint("MPLOOT host caught netId=%u pid=0x%X", netId, pid);
            return;
        }
    } else {
        if (itemMove(from, to, item, amount) == 0) {
            ok = true;
            deltaCount = 1;
            deltas[0].pid = pid;
            deltas[0].qty = plant ? -amount : amount;
            if (!plant && (item->flags & OBJECT_IN_RIGHT_HAND) != 0) {
                // Vanilla parity: stripping an equipped weapon refreshes the
                // critter's display art.
                target->fid = buildFid(FID_TYPE(target->fid), target->fid & 0xFFF,
                    animationTypeFromFid(target->fid), 0, target->rotation + 1);
            }
            target->flags &= ~OBJECT_EQUIPPED;
        } else {
            inventoryFormatMessage(plant ? 26 : 25, 0, msg, sizeof(msg));
        }
    }

    mpLootHostSendState(s, NET_LOOT_OP_MOVE, ok,
        msg[0] != '\0' ? msg : nullptr, deltas, deltaCount);
    if (ok) {
        mpLootHostRefreshOtherSessions(netId, s->targetNetId);
    }
    debugFilePrint("MPLOOT host move netId=%u pid=0x%X qty=%d roll=%d ok=%d",
        netId, pid, qty, doRoll, ok);
}

static void mpLootHostTakeAll(uint8_t netId)
{
    MpLootHostSession* s = &gLootHost[netId - 1];
    // Take-all is loot-only, matching the vanilla window's !_gIsSteal gate.
    if (!s->active || s->isSteal) {
        return;
    }
    Object* target = MpFindObjByNetId(s->targetNetId);
    if (target == nullptr) {
        s->active = false;
        return;
    }
    Object* avatar = gMpSession.players[netId - 1].obj;
    if (avatar == nullptr) {
        return;
    }

    // Vanilla parity: the weight gate runs before the forced move-all.
    int maxCarry = critterGetStat(avatar, STAT_CARRY_WEIGHT);
    int currentWeight = objectGetInventoryWeight(avatar);
    if (objectGetInventoryWeight(target) > maxCarry - currentWeight) {
        char msg[64] = { 0 };
        inventoryFormatMessage(31, 0, msg, sizeof(msg));
        mpLootHostSendState(s, NET_LOOT_OP_TAKE_ALL, false,
            msg[0] != '\0' ? msg : nullptr, nullptr, 0);
        return;
    }

    NetLootItem deltas[NET_LOOT_MAX_ITEMS];
    int deltaCount = 0;
    Inventory* inv = &target->data.inventory;
    while (inv->length > 0 && deltaCount < NET_LOOT_MAX_ITEMS) {
        InventoryItem* inventoryItem = &inv->items[0];
        Object* item = inventoryItem->item;
        int quantity = inventoryItem->quantity;
        if (itemMoveForce(target, avatar, item, quantity) == 0) {
            deltas[deltaCount].pid = item->pid;
            deltas[deltaCount].qty = quantity;
            deltaCount++;
        } else {
            break;
        }
    }
    mpLootHostSendState(s, NET_LOOT_OP_TAKE_ALL, true, nullptr, deltas, deltaCount);
    mpLootHostRefreshOtherSessions(netId, s->targetNetId);
    debugFilePrint("MPLOOT host take-all netId=%u moved=%d", netId, deltaCount);
}

void MpLootHostStart(uint8_t netId, Object* target, bool isSteal)
{
    if (!gMpActive || !gMpIsHost) {
        return;
    }
    if (netId == 0 || netId > NET_MAX_PLAYERS) {
        return;
    }
    MultiplayerPlayer* player = &gMpSession.players[netId - 1];
    if (!player->isConnected || player->obj == nullptr) {
        return;
    }
    if (target == nullptr || target == player->obj) {
        return;
    }
    // Players' own critters are never lootable.
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        if (gMpSession.players[i].isConnected && gMpSession.players[i].obj == target) {
            return;
        }
    }
    MpLootHostSession* s = &gLootHost[netId - 1];
    if (s->active) {
        return; // already looting
    }
    // Vanilla parity: steal is blocked in combat (902), loot is not; steal
    // targets are limited to items and critters.
    if (isSteal) {
        if (isInCombat()) {
            return;
        }
        if (PID_TYPE(target->pid) != OBJ_TYPE_ITEM && PID_TYPE(target->pid) != OBJ_TYPE_CRITTER) {
            return;
        }
    }

    s->active = true;
    s->netId = netId;
    s->isSteal = isSteal;
    s->targetNetId = MpGetObjNetId(target);
    if (s->targetNetId == 0) {
        s->active = false;
        return;
    }
    s->wasCaught = false;
    s->stealCount = 0;
    s->stealingXp = 0;
    s->stealXpBonus = 10;
    debugFilePrint("MPLOOT host start netId=%u isSteal=%d targetNetId=%u",
        netId, isSteal, s->targetNetId);

    if (objectWithinWalkDistance(player->obj, target)) {
        mpLootHostOpen(netId);
    } else {
        // Walk the avatar next to the target (visible to every client through
        // the player-state channel), then open on arrival.
        reg_anim_begin(ANIMATION_REQUEST_UNRESERVED);
        animationRegisterRunToObject(player->obj, target, -1, 0);
        animationRegisterCallbackForced(player->obj, target,
            (AnimationCallback*)mpLootHostArrived, -1);
        reg_anim_end();
    }
}

void MpLootOnHostPacket(const void* data, size_t dataLength, void* peer)
{
    if (!gMpActive || !gMpIsHost) {
        return;
    }
    if (dataLength != sizeof(NetLootCmdPayload)) {
        debugFilePrint("MPLOOT cmd rejected (bad length) len=%zu", dataLength);
        return;
    }
    const NetLootCmdPayload* p = static_cast<const NetLootCmdPayload*>(data);
    if (p->netId == 0 || p->netId > NET_MAX_PLAYERS) {
        return;
    }
    MultiplayerPlayer* player = &gMpSession.players[p->netId - 1];
    if (!player->isConnected || player->peer != peer) {
        debugFilePrint("MPLOOT cmd rejected (peer mismatch) netId=%u", p->netId);
        return;
    }
    if (p->targetNetId != gLootHost[p->netId - 1].targetNetId) {
        debugFilePrint("MPLOOT cmd rejected (target mismatch) netId=%u target=%u",
            p->netId, p->targetNetId);
        return;
    }

    switch (p->op) {
    case NET_LOOT_OP_MOVE:
        mpLootHostMove(p->netId, p->pid, p->qty);
        break;
    case NET_LOOT_OP_TAKE_ALL:
        mpLootHostTakeAll(p->netId);
        break;
    case NET_LOOT_OP_END:
        mpLootHostEnd(p->netId, nullptr);
        break;
    default:
        debugFilePrint("MPLOOT cmd unknown op=%u netId=%u", p->op, p->netId);
        break;
    }
}

void MpLootHostPlayerDisconnected(uint8_t netId)
{
    if (netId == 0 || netId > NET_MAX_PLAYERS) {
        return;
    }
    MpLootHostSession* s = &gLootHost[netId - 1];
    if (!s->active) {
        return;
    }
    s->active = false;
    debugFilePrint("MPLOOT host session closed (disconnect) netId=%u", netId);
}

// Closes every host session without awarding XP — the targets die with the
// old map, so any pending steal XP is lost exactly as vanilla would lose it
// on a forced window close.
void MpLootHostCloseAllSessions()
{
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        if (gLootHost[i].active) {
            gLootHost[i].active = false;
            debugFilePrint("MPLOOT host session closed (map change) netId=%u", gLootHost[i].netId);
        }
    }
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

struct MpLootClientState {
    bool active;
    bool isSteal;
    bool windowOpen; // the vanilla loot modal is currently running on the mirror
    Object* mirror;
    uint32_t targetNetId;
    bool dirty;
};

static MpLootClientState gLootClient = {};

static void mpLootClientMirrorClear(Object* mirror)
{
    if (mirror == nullptr || mirror->data.inventory.length == 0) {
        return;
    }
    Object* scratch = nullptr;
    if (objectCreateWithFidPid(&scratch, -1, PROTO_ID_JESSE_CONTAINER) == -1) {
        return;
    }
    itemMoveAll(mirror, scratch);
    objectDestroy(scratch, nullptr);
}

static void mpLootClientMirrorPopulate(Object* mirror, const NetLootItem* items, int count)
{
    if (mirror == nullptr) {
        return;
    }
    mpLootClientMirrorClear(mirror);
    for (int i = 0; i < count; i++) {
        Object* item = nullptr;
        if (objectCreateWithPid(&item, (int)items[i].pid) == -1) {
            continue;
        }
        itemAdd(mirror, item, items[i].qty);
    }
}

// Replays the authoritative avatar-side delta onto the local dude inventory
// (the host owns the real avatar; the local dude is only touched here).
static void mpLootClientApplyDudeDeltas(const NetLootItem* deltas, int count)
{
    if (gDude == nullptr || count <= 0) {
        return;
    }
    Object* scratch = nullptr;
    if (objectCreateWithFidPid(&scratch, -1, PROTO_ID_JESSE_CONTAINER) == -1) {
        return;
    }
    for (int i = 0; i < count; i++) {
        if (deltas[i].qty > 0) {
            Object* item = nullptr;
            if (objectCreateWithPid(&item, (int)deltas[i].pid) == -1) {
                continue;
            }
            itemAdd(gDude, item, deltas[i].qty);
        } else if (deltas[i].qty < 0) {
            int qty = -deltas[i].qty;
            Object* item = mpLootFindItemByPid(gDude, deltas[i].pid);
            if (item == nullptr) {
                continue;
            }
            int have = itemGetQuantity(gDude, item);
            if (qty > have) {
                qty = have;
            }
            if (qty > 0) {
                itemMoveForce(gDude, scratch, item, qty);
            }
        }
    }
    objectDestroy(scratch, nullptr);
}

// Full teardown: destroys the mirror and zeroes the state. Only safe when
// the vanilla loot modal is NOT running (window tail, or no window yet) —
// the modal's tail code touches the mirror object after the loop breaks.
static void mpLootClientCleanup()
{
    if (gLootClient.mirror != nullptr) {
        mpLootClientMirrorClear(gLootClient.mirror);
        objectDestroy(gLootClient.mirror, nullptr);
    }
    gLootClient = MpLootClientState{};
}

// Force-close without destroying the mirror: marks the session dead so the
// modal loop breaks at the next tick and its tail (MpLootLoopEnded) destroys
// the mirror. If no modal is running, tears down immediately.
static void mpLootClientCloseSession()
{
    gLootClient.active = false;
    if (!gLootClient.windowOpen) {
        mpLootClientCleanup();
    }
}

void MpLootOnClientPacket(const void* data, size_t dataLength)
{
    if (!gMpActive || !gMpIsClient) {
        return;
    }
    if (dataLength < sizeof(NetLootStatePayload)) {
        return;
    }
    const NetLootStatePayload* p = static_cast<const NetLootStatePayload*>(data);
    size_t expected = sizeof(NetLootStatePayload)
        + ((size_t)p->targetItemCount + p->dudeDeltaCount) * sizeof(NetLootItem);
    if (dataLength < expected || p->targetItemCount > NET_LOOT_MAX_ITEMS
        || p->dudeDeltaCount > NET_LOOT_MAX_ITEMS) {
        return;
    }
    if (p->netId != gMpSession.localNetId) {
        return; // only our own sessions
    }

    const NetLootItem* items = reinterpret_cast<const NetLootItem*>(
        static_cast<const uint8_t*>(data) + sizeof(NetLootStatePayload));
    const NetLootItem* deltas = items + p->targetItemCount;

    switch (p->op) {
    case NET_LOOT_OP_OPEN: {
        // The host never starts a session while one is active; a stray OPEN
        // is ignored rather than tearing down a running window.
        if (gLootClient.active) {
            debugFilePrint("MPLOOT client open ignored (session active)");
            return;
        }
        if (p->targetFid == 0 || p->targetPid == 0) {
            if (p->msgText[0] != '\0') {
                displayMonitorAddMessage(p->msgText);
            }
            return;
        }

        Object* mirror = nullptr;
        if (objectCreateWithFidPid(&mirror, (int)p->targetFid, (int)p->targetPid) == -1) {
            debugFilePrint("MPLOOT client mirror create failed fid=0x%X pid=0x%X",
                p->targetFid, p->targetPid);
            return;
        }
        mirror->flags |= (OBJECT_HIDDEN | OBJECT_NO_SAVE);
        mpLootClientMirrorPopulate(mirror, items, p->targetItemCount);

        gLootClient.active = true;
        gLootClient.isSteal = p->isSteal != 0
            && PID_TYPE(mirror->pid) == OBJ_TYPE_CRITTER
            && critterIsActive(mirror);
        gLootClient.mirror = mirror;
        gLootClient.targetNetId = p->targetNetId;
        gLootClient.dirty = false;
        debugFilePrint("MPLOOT client open netId=%u isSteal=%d mirror=%p targetNetId=%u",
            p->netId, gLootClient.isSteal, (void*)mirror, p->targetNetId);

        if (!p->lastOk) {
            // No window was ever opened — safe to tear down immediately.
            if (p->msgText[0] != '\0') {
                displayMonitorAddMessage(p->msgText);
            }
            mpLootClientCleanup();
            return;
        }

        gLootClient.windowOpen = true;
        int openResult;
        if (gLootClient.isSteal) {
            openResult = inventoryOpenStealing(gDude, mirror);
        } else {
            openResult = inventoryOpenLooting(gDude, mirror);
        }
        if (openResult != 0) {
            // The window never ran (creation failure) — tear down now.
            debugFilePrint("MPLOOT client window failed rc=%d", openResult);
            mpLootClientCloseSession();
        }
        break;
    }
    case NET_LOOT_OP_MOVE:
    case NET_LOOT_OP_TAKE_ALL: {
        if (!gLootClient.active || p->targetNetId != gLootClient.targetNetId) {
            return;
        }
        mpLootClientApplyDudeDeltas(deltas, p->dudeDeltaCount);
        mpLootClientMirrorPopulate(gLootClient.mirror, items, p->targetItemCount);
        gLootClient.dirty = true;
        if (p->msgText[0] != '\0') {
            displayMonitorAddMessage(p->msgText);
        }
        break;
    }
    case NET_LOOT_OP_END: {
        if (!gLootClient.active) {
            return;
        }
        if (p->msgText[0] != '\0') {
            displayMonitorAddMessage(p->msgText);
        }
        // Session dead — the modal loop breaks at its next tick and its tail
        // destroys the mirror. If no modal is running, tears down now.
        mpLootClientCloseSession();
        debugFilePrint("MPLOOT client closed netId=%u", p->netId);
        break;
    }
    default:
        break;
    }
}

void MpLootOnClientReset()
{
    if (gLootClient.active || gLootClient.mirror != nullptr) {
        debugFilePrint("MPLOOT client session reset");
        mpLootClientCloseSession();
    }
}

bool MpLootSessionOpen()
{
    return gMpActive && gMpIsClient && gLootClient.active;
}

bool MpLootLoopTick()
{
    if (!gMpActive) {
        return true;
    }
    if (gMpIsHost) {
        return true; // the host's own loot window is vanilla
    }
    MpTick();
    if (!gLootClient.active) {
        return false;
    }
    if (gMpSession.state != MP_STATE_CLIENT_PLAYING) {
        // Host died, kicked us, or left the map mid-session — the mirror
        // (and the local dude deltas it references) is stale; close the
        // session now, the modal tail destroys the mirror.
        mpLootClientCloseSession();
        return false;
    }
    return true;
}

bool MpLootConsumeDirty()
{
    if (!gMpActive || gMpIsHost || !gLootClient.active) {
        return false;
    }
    bool dirty = gLootClient.dirty;
    gLootClient.dirty = false;
    return dirty;
}

// The vanilla loot modal loop has exited — the window tail has finished all
// mirror touches, so this is the only place the mirror may be destroyed.
// Sends END when the session was still ours (the host may have closed it
// first via a caught steal, host death, or map change).
void MpLootLoopEnded()
{
    if (!gMpActive || gMpIsHost) {
        return;
    }
    if (gLootClient.active) {
        NetLootCmdPayload p{};
        p.op = NET_LOOT_OP_END;
        p.netId = gMpSession.localNetId;
        p.targetNetId = gLootClient.targetNetId;
        NetSendPacket(gMpSession.hostPeer, NET_CHANNEL_RELIABLE, NET_PKT_LOOT_CMD, &p, sizeof(p));
        debugFilePrint("MPLOOT client ended (local) netId=%u", gMpSession.localNetId);
    }
    mpLootClientCleanup();
}

void MpLootClientSendMove(uint32_t pid, int32_t qty)
{
    if (!gLootClient.active) {
        return;
    }
    NetLootCmdPayload p{};
    p.op = NET_LOOT_OP_MOVE;
    p.netId = gMpSession.localNetId;
    p.targetNetId = gLootClient.targetNetId;
    p.pid = pid;
    p.qty = qty;
    NetSendPacket(gMpSession.hostPeer, NET_CHANNEL_RELIABLE, NET_PKT_LOOT_CMD, &p, sizeof(p));
    debugFilePrint("MPLOOT client move pid=0x%X qty=%d", pid, qty);
}

void MpLootClientSendTakeAll()
{
    if (!gLootClient.active || gLootClient.isSteal) {
        return;
    }
    NetLootCmdPayload p{};
    p.op = NET_LOOT_OP_TAKE_ALL;
    p.netId = gMpSession.localNetId;
    p.targetNetId = gLootClient.targetNetId;
    NetSendPacket(gMpSession.hostPeer, NET_CHANNEL_RELIABLE, NET_PKT_LOOT_CMD, &p, sizeof(p));
    debugFilePrint("MPLOOT client take-all");
}

} // namespace fallout
