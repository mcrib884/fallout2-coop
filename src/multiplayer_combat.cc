// Multiplayer combat sync.
//
// The host is the only combat simulator: it runs the vanilla _combat sequence
// and resolves every player intent against its authoritative copies. Clients
// mirror combat via broadcasts: they never run the vanilla _combat, never run
// local AI, and send intents (move / attack) that the host executes.
//
// Turn flow:
//   host round loop reaches a remote player's critter
//     -> COMBAT_TURN_START(netId, ap, maxAp) broadcast
//     -> host waits (scoped pump: network + broadcasts + render only)
//     -> acting client runs its co-op turn loop, sends intents + TURN_END
//   combat start: any machine may request (client sends START_REQUEST, host
//     starts _combat and broadcasts STARTED)
//   combat end: any machine may press RETURN -> END_REQUEST -> host runs the
//     vanilla enemy check -> DENIED or ENDED broadcast

#include "multiplayer_combat.h"
#include <algorithm>
#include <string.h>
#include <vector>

#include "animation.h"
#include "color.h"
#include "combat.h"
#include "critter.h"
#include "debug.h"
#include "display_monitor.h"
#include "game.h"
#include "game_mouse.h"
#include "input.h"
#include "interface.h"
#include "item.h"
#include "kb.h"
#include "map.h"
#include "memory.h"
#include "multiplayer.h"
#include "multiplayer_debug.h"
#include "multiplayer_dialog.h"
#include "multiplayer_loot.h"
#include "multiplayer_vote.h"
#include "multiplayer_log.h"
#include "object.h"
#include "perk.h"
#include "proto.h"
#include "scripts.h"
#include "tile.h"
#include "skill.h"
#include "stat.h"
#include "text_object.h"
#include "svga.h"
#include "text_font.h"
#include "trait.h"
#include "window_manager.h"
#include "window_manager_private.h"

namespace fallout {

MpCombatState gMpCombat = {};

// Max length of a mirrored display-monitor line (including the NUL).
#define MP_COMBAT_MESSAGE_MAX_LENGTH 255

// Host-side intent queue: packet callbacks only enqueue; the scoped pumps
// drain and resolve. Never resolve inside a net callback.
struct MpCombatQueuedCmd {
    uint8_t netId;
    NetCombatCmdPayload payload;
};
static std::vector<MpCombatQueuedCmd> gCombatCmdQueue;

// HUD player cards (windows over the bottom interface bar).
#define MP_COMBAT_CARD_WIDTH 128
#define MP_COMBAT_CARD_HEIGHT 30
#define MP_COMBAT_CARD_GAP 4
static int gCombatCardWindows[NET_MAX_PLAYERS];
static uint32_t gCombatLastCardUpdate = 0;

// Client-side acting-NPC outline mirror: vanilla outlines the critter whose
// turn it is with the red combat outline; the client never runs the vanilla
// turn loop, so the host's TURN_START (netId=0, targetNetId=acting NPC) is
// mirrored into objectEnableOutline here.
static uint32_t gMpClientActingNpcNetId = 0;

// Client-initiated ambush: the requesting client's critter and the netId of
// the object it attacked. The host builds the CombatStartData from these when
// the deferred start runs, so the ambushing player acts first (vanilla
// attacker-first rule) instead of the host always going first.
static Object* gPendingStartAttacker = nullptr;
static uint32_t gPendingStartTargetNetId = 0;

static void mpCombatBroadcast(uint8_t type, const void* data, size_t dataLength)
{
    if (gMpSession.enetHost != nullptr) {
        NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE, type, data, dataLength);
    }
}

static void mpCombatSend(uint8_t type, const void* data, size_t dataLength)
{
    if (gMpIsClient && gMpSession.hostPeer != nullptr) {
        NetSendPacket(gMpSession.hostPeer, NET_CHANNEL_RELIABLE, type, data, dataLength);
    }
}

// ENet zero-length client->host packets were observed never reaching the
// host's receive dispatch (vote casts with a 1-byte payload always arrived).
// All client->host combat packets therefore carry a single reserved byte.
static uint8_t gMpCombatDummy = 0;

static void mpCombatSendNonEmpty(uint8_t type)
{
    mpCombatSend(type, &gMpCombatDummy, sizeof(gMpCombatDummy));
}

static bool mpCombatVoteOrTransitionActive()
{
    return gVoteSession.state != VOTE_STATE_NONE;
}

void MpCombatReset()
{
    gMpCombat = {};
    gCombatCmdQueue.clear();
    gPendingStartAttacker = nullptr;
    gPendingStartTargetNetId = 0;
    gMpClientActingNpcNetId = 0;
}

bool MpCombatIsActive()
{
    return gMpCombat.inCombat;
}

// Jinxed is a global curse in vanilla: the trait-holder's presence poisons
// every combatant's misses. A remote player carrying it must do the same.
bool MpCombatAnyPlayerHasJinxed()
{
    if (!gMpActive) {
        return false;
    }
    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        MultiplayerPlayer* player = &gMpSession.players[index];
        if (!player->isConnected || player->obj == nullptr || player->obj == gDude) {
            continue;
        }
        if (traitIsSelectedFor(player->obj, TRAIT_JINXED)
            || perkHasRank(player->obj, PERK_JINXED)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// HUD player cards
// ---------------------------------------------------------------------------

static void mpCombatDestroyCards()
{
    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        if (gCombatCardWindows[index] != -1) {
            windowDestroy(gCombatCardWindows[index]);
            gCombatCardWindows[index] = -1;
        }
    }
}

static void mpCombatCreateCards()
{
    mpCombatDestroyCards();
    int cardIndex = 0;
    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        gCombatCardWindows[index] = -1;
        MultiplayerPlayer* player = &gMpSession.players[index];
        if (!player->isConnected || player->netId == 0) {
            continue;
        }
        int x = 4 + cardIndex * (MP_COMBAT_CARD_WIDTH + MP_COMBAT_CARD_GAP);
        // Right above the bottom interface bar, never over it.
        int y = screenGetHeight() - INTERFACE_BAR_HEIGHT - MP_COMBAT_CARD_HEIGHT - 8;
        int win = windowCreate(x, y, MP_COMBAT_CARD_WIDTH, MP_COMBAT_CARD_HEIGHT,
            COLOR_BLACK, WINDOW_DONT_MOVE_TOP);
        if (win == -1) {
            continue;
        }
        // No windowDrawBorder(): the vanilla bevel is replaced by the
        // per-player colored border drawn in mpCombatUpdateCards.
        gCombatCardWindows[index] = win;
        MpLog(MP_LOG_COMBAT, "card created win=%d netId=%u x=%d y=%d", win, player->netId, x, y);
        cardIndex++;
        if (cardIndex >= 5) {
            break;
        }
    }
    gCombatLastCardUpdate = 0;
}

static void mpCombatUpdateCards()
{
    // The card text must not depend on whatever font the frame left current
    // (MpTick runs outside the interface's font scope — a large font would
    // clip the stats line out of the card entirely). Font 101 is the
    // standard interface font; the y positions are derived from its line
    // height so both lines always fit the 30px card.
    ScopedFont cardFont(101);
    int lineHeight = fontGetLineHeight();

    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        int win = gCombatCardWindows[index];
        if (win == -1) {
            continue;
        }
        MultiplayerPlayer* player = &gMpSession.players[index];
        if (player == nullptr || player->netId == 0) {
            continue;
        }

        Object* obj = nullptr;
        if (player->isLocal) {
            obj = gDude;
        } else if (gMpIsHost) {
            obj = player->obj;
        } else {
            // The state-sync object (player->obj) is the live one the player
            // states animate; the netId table is wiped and rebuilt on every
            // map change and may hold a stale clone instead. Prefer the live
            // pointer, fall back to the netId lookup.
            obj = player->obj != nullptr ? player->obj : MpFindObjByNetId(player->objNetId);
        }

        if (obj == nullptr && !player->isLocal) {
            static uint32_t gMpCardObjMissLogTick = 0;
            uint32_t nowTicks = getTicks();
            if (nowTicks - gMpCardObjMissLogTick > 2000) {
                gMpCardObjMissLogTick = nowTicks;
                MpLogAlways(MP_LOG_COMBAT, "card obj missing netId=%u objNet=%u pObj=%p",
                    player->netId, player->objNetId, (void*)player->obj);
            }
        }

        unsigned char* buffer = windowGetBuffer(win);
        bufferFill(buffer, MP_COMBAT_CARD_WIDTH, MP_COMBAT_CARD_HEIGHT,
            MP_COMBAT_CARD_WIDTH, COLOR_BLACK);

        // Per-player border color, stable per netId. The active player's card
        // switches to a thicker white border so the current turn reads at a
        // glance alongside the ">>" name marker.
        bool isActiveTurn = gMpCombat.whoseTurn == player->netId;
        int borderColor = MpPlayerColorFor(player->obj);
        if (borderColor < 0) {
            switch ((player->netId - 1) % 5) {
            case 0: borderColor = COLOR_LIGHT_GOLD; break;
            case 1: borderColor = COLOR_CYAN; break;
            case 2: borderColor = COLOR_LIGHT_GREEN_3; break;
            case 3: borderColor = COLOR_LIGHT_PINK; break;
            case 4: borderColor = COLOR_LIGHT_ORANGE; break;
            default: borderColor = COLOR_LIGHT_GOLD; break;
            }
        }
        if (isActiveTurn) {
            borderColor = COLOR_WHITE;
        }
        windowDrawRect(win, 0, 0, MP_COMBAT_CARD_WIDTH - 1, MP_COMBAT_CARD_HEIGHT - 1,
            borderColor);
        if (isActiveTurn) {
            windowDrawRect(win, 1, 1, MP_COMBAT_CARD_WIDTH - 2, MP_COMBAT_CARD_HEIGHT - 2,
                borderColor);
        }

        int color = isActiveTurn ? COLOR_WHITE : COLOR_LIGHT_GREY;

        // The local player's slot never gets a name from the profile channel
        // (only remote profiles arrive that way); fall back to the critter.
        const char* name = player->name;
        if ((name == nullptr || name[0] == '\0') && obj != nullptr) {
            name = critterGetName(obj);
        }
        if (name == nullptr || name[0] == '\0') {
            name = "?";
        }

        char text[64];
        if (gMpCombat.whoseTurn == player->netId) {
            snprintf(text, sizeof(text), ">> %s", name);
        } else {
            snprintf(text, sizeof(text), "   %s", name);
        }
        windowDrawText(win, text, 0, 4, 2, color);

        if (obj != nullptr) {
            int hp = critterGetStat(obj, STAT_CURRENT_HIT_POINTS);
            int ac = critterGetStat(obj, STAT_ARMOR_CLASS);
            int ap = obj->data.critter.combat.ap;
            snprintf(text, sizeof(text), "HP %d AC %d AP %d", hp, ac, ap);
            windowDrawText(win, text, 0, 4, 2 + lineHeight, color);
        }

        windowRefresh(win);
    }
}

// ---------------------------------------------------------------------------
// host hooks (combat.cc)
// ---------------------------------------------------------------------------

void MpCombatOnStarted()
{
    if (!gMpIsHost || !gMpActive) {
        return;
    }
    if (gMpCombat.inCombat) {
        MpLogAlways(MP_LOG_COMBAT, "start rejected (already in combat)");
        return;
    }
    // A client can be mid-loot when the host's script starts combat. The
    // client's own loot modal closes itself (combat-active check in
    // MpLootLoopTick), but release the host-side session now so a lost END
    // can never strand the session across the fight.
    MpLootHostCloseAllSessions();
    // Same for an active dialogue: the blocking combat pump takes over the
    // host loop and the director tick never runs again, so broadcast the END
    // here or every client's dialogue modal stays open and defers all combat
    // packets (stuck convo, dead buttons).
    MpDialogHostAbortCombat();
    gMpCombat.inCombat = true;
    gMpCombat.whoseTurn = 0;
    gMpCombat.waitingForTurnEnd = 0;
    // A stale EXIT_REQUESTED would insta-end this fresh combat at the first
    // remote-turn wait. The vanilla clear lives in the dude's own turn, which
    // never runs when the round is remote — consume the flag here instead.
    gCombatState &= ~COMBAT_STATE_EXIT_REQUESTED;
    mpCombatBroadcast(NET_PKT_COMBAT_STARTED, nullptr, 0);
    mpCombatCreateCards();
    MpLog(MP_LOG_COMBAT, "host combat started, STARTED broadcast");
}

void MpCombatOnEnded()
{
    if (!gMpIsHost || !gMpActive) {
        return;
    }
    gMpCombat.inCombat = false;
    gMpCombat.whoseTurn = 0;
    gMpCombat.waitingForTurnEnd = 0;
    gMpCombat.endRequestPending = false;
    gCombatCmdQueue.clear();
    // Consume the end-request flag on every combat end: the vanilla clear is
    // inside the dude's own turn (combat.cc _combat_input), which never runs
    // when the round ended from a remote player's turn. Left set, it would
    // insta-end the NEXT combat at its first remote-turn wait and re-trigger
    // the initiating script forever.
    gCombatState &= ~COMBAT_STATE_EXIT_REQUESTED;
    // The vanilla _combat_over restores the mouse MODE but the WAIT_WATCH
    // cursor set at combat begin can survive it; the host must never be left
    // looking like a spectator after combat ends.
    gameMouseSetCursor(MOUSE_CURSOR_ARROW);
    gameMouseSetMode(GAME_MOUSE_MODE_MOVE);
    mpCombatBroadcast(NET_PKT_COMBAT_ENDED, nullptr, 0);
    mpCombatDestroyCards();
    MpLog(MP_LOG_COMBAT, "host combat ended, ENDED broadcast");
}

uint8_t MpCombatGetCritterPlayerNetId(Object* critter)
{
    if (critter == nullptr || !gMpIsHost || !gMpActive) {
        return 0;
    }
    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        MultiplayerPlayer* player = &gMpSession.players[index];
        if (player->isConnected && !player->isLocal && player->obj == critter) {
            return player->netId;
        }
    }
    return 0;
}

int MpCombatHostRemoteTurn(Object* critter, uint8_t netId)
{
    if (critter == nullptr) {
        return 0;
    }

    int ap = critter->data.critter.combat.ap;
    int maxAp = critterGetStat(critter, STAT_MAXIMUM_ACTION_POINTS);

    // Co-op: the vanilla free-move global is only ever set for the host's own
    // dude (combat.cc _combat_turn). While a remote player's turn resolves on
    // the host, the move machinery consumes free points first (animation.cc
    // _object_move) — point it at the REMOTE avatar's own Bonus Move, or the
    // avatar's real AP is charged for every tile while its free points go
    // unused (and the owner's mirror sees a phantom golden bar).
    _combat_free_move = 2 * perkGetRank(critter, PERK_BONUS_MOVE);

    NetCombatTurnStartPayload payload;
    payload.netId = netId;
    payload.ap = (uint16_t)std::clamp(ap, 0, 65535);
    payload.maxAp = (uint16_t)std::clamp(maxAp, 0, 65535);
    payload.targetNetId = 0;
    mpCombatBroadcast(NET_PKT_COMBAT_TURN_START, &payload, sizeof(payload));

    gMpCombat.whoseTurn = netId;
    gMpCombat.waitingForTurnEnd = netId;
    gMpCombat.turnEndPending = false;
    gMpCombat.turnActive = false;
    MpLog(MP_LOG_COMBAT, "host remote turn begin netId=%u ap=%d maxAp=%d", netId, ap, maxAp);

    MultiplayerPlayer* player = &gMpSession.players[netId - 1];

    while (gMpCombat.waitingForTurnEnd == netId && gMpCombat.inCombat
        && !gMpCombat.turnEndPending) {
        // The pump dispatches packets (queuing cmds) and then drains the
        // queue; the turnEndPending flag is only checked here, at the top of
        // the next iteration, so a TURN_END that raced in with the player's
        // final actions still lets those actions resolve first.
        MpCombatPump();
        mpCombatUpdateCards();

        int key = inputGetInput();
        if (key == KEY_RETURN) {
            // The host is a spectator during a remote turn but may still end
            // combat with RETURN (the vanilla enemy check runs here).
            gMpCombat.endRequestPending = true;
            gMpCombat.endRequesterNetId = gMpSession.localNetId;
        }

        if ((gCombatState & COMBAT_STATE_EXIT_REQUESTED) != 0) {
            MpLog(MP_LOG_COMBAT, "host remote turn exit requested");
            break;
        }
        if (player == nullptr || !player->isConnected) {
            MpLog(MP_LOG_COMBAT, "remote turn netId=%u disconnected, skipping", netId);
            break;
        }
        if (critterIsDead(critter)) {
            MpLog(MP_LOG_COMBAT, "remote turn critter died, ending turn");
            break;
        }

        renderPresent();
        sharedFpsLimiter.mark();
        sharedFpsLimiter.throttle();
    }

    gMpCombat.waitingForTurnEnd = 0;
    gMpCombat.whoseTurn = 0;
    gMpCombat.turnEndPending = false;
    // The remote avatar's free move must not leak into the host's own next
    // turn (the vanilla dude-turn re-derives its own value at combat.cc:3373).
    _combat_free_move = 0;
    MpLog(MP_LOG_COMBAT, "host remote turn end netId=%u", netId);

    return (gCombatState & COMBAT_STATE_EXIT_REQUESTED) != 0 ? -1 : 0;
}

// ---------------------------------------------------------------------------
// host intent resolution
// ---------------------------------------------------------------------------

// The client resolves its own move clicks optimistically (its local dude
// walks the full clicked path), but the host is authoritative. Every
// resolution is sent back so the client can snap its local dude when the
// outcome diverges from the clicked destination (reject or occupancy
// truncation) — the vanilla targeting UI must read the avatar's true tile.
static void mpCombatSendMoveResult(MultiplayerPlayer* player, int tile, int elevation)
{
    if (player == nullptr || player->peer == nullptr) {
        return;
    }
    NetCombatMoveResultPayload payload;
    memset(&payload, 0, sizeof(payload));
    payload.netId = player->netId;
    payload.tile = tile;
    payload.elevation = elevation;
    NetSendPacket(player->peer, NET_CHANNEL_RELIABLE,
        NET_PKT_COMBAT_MOVE_RESULT, &payload, sizeof(payload));
}

// Reply to a refused combat attack. The acting client already played its
// swing locally, so every refusal MUST be answered — a silent rejection is
// exactly the "nothing I do does anything until I end my turn" stall. The
// rejection carries the attacker's authoritative position so the client can
// reconcile its local avatar (its optimistic position runs ahead of the
// host's, and range checks then diverge).
static void mpCombatSendAttackRejected(MultiplayerPlayer* player,
    uint32_t targetNetId, int rc)
{
    if (player == nullptr || player->peer == nullptr) {
        return;
    }
    NetPlayerEventPayload rejected;
    rejected.opcode = NET_PLAYER_EVENT_ATTACK_REJECTED;
    rejected.netId = player->netId;
    rejected.arg1 = player->obj != nullptr ? player->obj->tile : -1;
    rejected.arg2 = player->obj != nullptr ? player->obj->elevation : -1;
    rejected.arg3 = targetNetId;
    rejected.arg4 = 0;
    NetSendPacket(player->peer, NET_CHANNEL_RELIABLE,
        NET_PKT_PLAYER_EVENT, &rejected, sizeof(rejected));
    MpLogAlways(MP_LOG_COMBAT, "attack rejected netId=%u targetNetId=%u rc=%d tile=%d elev=%d",
        player->netId, targetNetId, rc, rejected.arg1, rejected.arg2);
}

static void mpCombatResolveMove(MultiplayerPlayer* player, const NetCombatCmdPayload* cmd)
{
    Object* critter = player->obj;
    if (critter == nullptr) {
        MpLogAlways(MP_LOG_COMBAT, "move rejected (no critter) netId=%u", player->netId);
        return;
    }
    if (!hexGridTileIsValid(cmd->tile) || !elevationIsValid(cmd->elevation)) {
        MpLogAlways(MP_LOG_COMBAT, "move rejected (bad tile/elev) netId=%u tile=%d elev=%d",
            player->netId, cmd->tile, cmd->elevation);
        mpCombatSendMoveResult(player, critter->tile, critter->elevation);
        return;
    }
    if (cmd->elevation != critter->elevation) {
        MpLogAlways(MP_LOG_COMBAT, "move rejected (elevation) netId=%u", player->netId);
        mpCombatSendMoveResult(player, critter->tile, critter->elevation);
        return;
    }
    int ap = critter->data.critter.combat.ap;
    if (ap <= 0) {
        MpLogAlways(MP_LOG_COMBAT, "move rejected (no AP) netId=%u", player->netId);
        mpCombatSendMoveResult(player, critter->tile, critter->elevation);
        return;
    }

    // Co-op: never resolve onto a tile occupied by another critter (see
    // MpTruncateDestinationAtOccupant) — stacked avatars break targeting.
    int targetTile = MpTruncateDestinationAtOccupant(critter, cmd->tile, cmd->elevation);
    if (targetTile != cmd->tile) {
        MpLog(MP_LOG_COMBAT, "move truncated netId=%u tile=%d->%d (occupied)",
            player->netId, cmd->tile, targetTile);
    }

    bool isRun = (cmd->reserved & 0x01) != 0;
    int moveRc = -1;
    reg_anim_begin(ANIMATION_REQUEST_RESERVED);
    if (isRun) {
        moveRc = animationRegisterRunToTile(critter, targetTile, cmd->elevation, ap, 0);
    } else {
        moveRc = animationRegisterMoveToTile(critter, targetTile, cmd->elevation, ap, 0);
    }
    reg_anim_end();

    if (moveRc != 0) {
        // The avatar is busy (attack batch: swing + target slide/death) or
        // the path is invalid — it did NOT move. Never pretend it resolved:
        // the acting client already walked optimistically and must snap back
        // to the authoritative position (the post-kill desync). The queue
        // deferral catches most of these earlier; this is the safety net.
        MpLogAlways(MP_LOG_COMBAT, "move registration failed netId=%u tile=%d busy=%d",
            player->netId, targetTile, animationIsBusy(critter) != 0 ? 1 : 0);
        mpCombatSendMoveResult(player, critter->tile, critter->elevation);
        return;
    }

    // Vanilla parity: running cancels sneaking unless the Silent Running perk
    // (same rule as mpHostRegisterPlayerMovement). Combat move intents flow
    // through here, not through the free-move path, so the avatar's proto
    // flag must be cleared here too — otherwise the player-state broadcast
    // keeps re-enabling the client's sneak every frame.
    if (isRun && perkGetRank(critter, PERK_SILENT_RUNNING) == 0) {
        Proto* playerProto = nullptr;
        if (protoGetProto(critter->pid, &playerProto) == 0
            && (playerProto->critter.data.flags & (1 << DUDE_STATE_SNEAKING)) != 0) {
            playerProto->critter.data.flags &= ~(1 << DUDE_STATE_SNEAKING);
            MpLog(MP_LOG_COMBAT, "run cancelled sneak netId=%u", player->netId);
        }
    }

    mpCombatSendMoveResult(player, targetTile, cmd->elevation);
    MpLog(MP_LOG_COMBAT, "move resolved netId=%u tile=%d elev=%d run=%d ap=%d free=%d",
        player->netId, targetTile, cmd->elevation, isRun ? 1 : 0,
        critter->data.critter.combat.ap, _combat_free_move);
}

static void mpCombatResolveAttack(MultiplayerPlayer* player, const NetCombatCmdPayload* cmd)
{
    Object* critter = player->obj;
    if (critter == nullptr) {
        MpLogAlways(MP_LOG_COMBAT, "attack rejected (no critter) netId=%u", player->netId);
        mpCombatSendAttackRejected(player, cmd->targetNetId, -1);
        return;
    }
    Object* target = MpFindObjByNetId(cmd->targetNetId);
    if (target == nullptr) {
        MpLogAlways(MP_LOG_COMBAT, "attack rejected (no target) netId=%u targetNetId=%u",
            player->netId, cmd->targetNetId);
        mpCombatSendAttackRejected(player, cmd->targetNetId, -1);
        return;
    }
    if (target->elevation != critter->elevation) {
        MpLogAlways(MP_LOG_COMBAT, "attack rejected (elevation) netId=%u", player->netId);
        mpCombatSendAttackRejected(player, cmd->targetNetId, -1);
        return;
    }
    int ap = critter->data.critter.combat.ap;
    if (ap <= 0) {
        MpLogAlways(MP_LOG_COMBAT, "attack rejected (no AP) netId=%u", player->netId);
        mpCombatSendAttackRejected(player, cmd->targetNetId, -1);
        return;
    }

    HitMode hitMode = (HitMode)cmd->hitMode;
    HitLocation hitLocation = (HitLocation)cmd->hitLocation;
    // The script combat's start-data override (_gcsd) is still live inside
    // the initiating turn's pump; it belongs to the vanilla initiator and
    // its min/max clamp would zero this remote attack's damage. Null it for
    // the resolve and restore right after.
    CombatStartData* savedGcsd = MpCombatSwapStartData(nullptr);
    int rc = _combat_attack(critter, target, hitMode, hitLocation);
    MpCombatSwapStartData(savedGcsd);
    if (rc == 0 && player->peer != nullptr) {
        // The outcome is computed synchronously (damage lands at the strike
        // frame); send it to the attacking client so it replays the hit
        // feedback with the authoritative numbers instead of its own roll.
        int damage = 0;
        int attackerFlags = 0;
        int defenderFlags = 0;
        MpGetLastAttackResult(&damage, &attackerFlags, &defenderFlags);
        NetPlayerEventPayload result;
        result.opcode = NET_PLAYER_EVENT_ATTACK_RESULT;
        result.netId = player->netId;
        result.arg1 = damage;
        result.arg2 = attackerFlags;
        result.arg3 = defenderFlags;
        result.arg4 = cmd->targetNetId;
        NetSendPacket(player->peer, NET_CHANNEL_RELIABLE,
            NET_PKT_PLAYER_EVENT, &result, sizeof(result));
        // Diagnostic: the damage formula halves the roll and subtracts the
        // target's DT — a 0 here means the attacker's melee/unarmed damage
        // resolved to 0, or the DT absorbed everything.
        Object* lastWeapon = MpGetLastAttackWeapon();
        DamageType resolvedType = weaponGetDamageType(critter, lastWeapon);
        int targetDt = target != nullptr
            ? critterGetStat(target, STAT_DAMAGE_THRESHOLD + resolvedType) : 0;
        int targetDr = target != nullptr
            ? critterGetStat(target, STAT_DAMAGE_RESISTANCE + resolvedType)
            : 0;
        int attackerUnarmed = skillGetValue(critter, SKILL_UNARMED);
        MpLog(MP_LOG_COMBAT, "attack resolved netId=%u targetNetId=%u mode=%d loc=%d dmg=%d flags=0x%X melee=%d weapon=%p wpnPid=0x%X dt=%d dr=%d unarmed=%d str=%d",
            player->netId, cmd->targetNetId, (int)hitMode, (int)hitLocation,
            damage, attackerFlags,
            critterGetStat(critter, STAT_MELEE_DAMAGE),
            (void*)lastWeapon,
            lastWeapon != nullptr ? lastWeapon->pid : 0,
            targetDt, targetDr,
            attackerUnarmed,
            critterGetStat(critter, STAT_STRENGTH));
    } else if (player->peer != nullptr) {
        // The attack was refused on the host (out of range / invalid state).
        // Answer anyway: the acting client already played its swing and needs
        // the authoritative position to reconcile (see
        // mpCombatSendAttackRejected).
        mpCombatSendAttackRejected(player, cmd->targetNetId, rc);
    } else {
        MpLog(MP_LOG_COMBAT, "attack resolved (no result sent) netId=%u rc=%d",
            player->netId, rc);
    }
}

static void mpCombatDrainQueue()
{
    if (gCombatCmdQueue.empty()) {
        return;
    }

    // The acting avatar is mid-animation (attack batch: swing + target
    // knockback slide + death). A move/attack registered now would be
    // rejected as busy and silently dropped — the client already played its
    // copy and would keep walking while the host's avatar never moves (the
    // post-kill desync). Defer the queue until the batch settles; the pump
    // runs every frame and retries. Order is preserved: everything behind
    // the first command waits too, so an attack never resolves from a
    // position its preceding move has not reached yet.
    uint8_t owner = gCombatCmdQueue.front().netId;
    if (owner >= 1 && owner <= NET_MAX_PLAYERS) {
        MultiplayerPlayer* ownerPlayer = &gMpSession.players[owner - 1];
        if (ownerPlayer->isConnected && ownerPlayer->obj != nullptr
            && animationIsBusy(ownerPlayer->obj)) {
            static uint32_t gLastQueueDeferLogTick = 0;
            uint32_t nowTicks = getTicks();
            if (nowTicks - gLastQueueDeferLogTick > 500) {
                gLastQueueDeferLogTick = nowTicks;
                MpLog(MP_LOG_COMBAT, "cmd queue deferred (avatar busy) netId=%u cmds=%zu",
                    owner, gCombatCmdQueue.size());
            }
            return;
        }
    }

    std::vector<MpCombatQueuedCmd> queue;
    queue.swap(gCombatCmdQueue);

    for (const MpCombatQueuedCmd& queued : queue) {
        if (queued.netId != gMpCombat.waitingForTurnEnd) {
            MpLogAlways(MP_LOG_COMBAT, "cmd rejected (not turn owner) netId=%u waiting=%u",
                queued.netId, gMpCombat.waitingForTurnEnd);
            continue;
        }
        MultiplayerPlayer* player = &gMpSession.players[queued.netId - 1];
        if (player == nullptr || !player->isConnected) {
            continue;
        }
        switch (queued.payload.cmd) {
        case NET_COMBAT_CMD_MOVE:
            mpCombatResolveMove(player, &queued.payload);
            break;
        case NET_COMBAT_CMD_ATTACK:
            mpCombatResolveAttack(player, &queued.payload);
            break;
        default:
            MpLog(MP_LOG_COMBAT, "unknown cmd %d netId=%u", queued.payload.cmd, queued.netId);
            break;
        }
    }
}

static void mpCombatDrainEndRequest()
{
    if (!gMpCombat.endRequestPending) {
        return;
    }
    gMpCombat.endRequestPending = false;
    if (!gMpIsHost || !gMpActive || !gMpCombat.inCombat) {
        return;
    }
    combatAttemptEndCoop();
    if ((gCombatState & COMBAT_STATE_EXIT_REQUESTED) == 0) {
        // The vanilla check refused (hostiles nearby): tell the requester.
        if (gMpCombat.endRequesterNetId != gMpSession.localNetId) {
            ENetPeer* peer = nullptr;
            int index = gMpCombat.endRequesterNetId - 1;
            if (index >= 0 && index < NET_MAX_PLAYERS) {
                peer = gMpSession.players[index].peer;
            }
            if (peer != nullptr) {
                NetSendPacket(peer, NET_CHANNEL_RELIABLE, NET_PKT_COMBAT_END_DENIED, nullptr, 0);
                MpLogAlways(MP_LOG_COMBAT, "end denied sent to netId=%u", gMpCombat.endRequesterNetId);
            }
        } else {
            MpLogAlways(MP_LOG_COMBAT, "host end combat refused (hostiles nearby)");
        }
    }
}

// ---------------------------------------------------------------------------
// network pump (safe inside blocking combat loops)
// ---------------------------------------------------------------------------

void MpCombatPump()
{
    // Off-screen player indicators and the camera drag: the blocking combat
    // loops suspend the main loop (which normally drives both), so pump them
    // here too — before the client early-return below, since the client's
    // blocking turn loop is exactly where the main loop is suspended.
    MpDrawPlayerIndicators();
    gameMouseCameraDragTick();

    if (!gMpActive || gMpSession.enetHost == nullptr) {
        return;
    }
    MpPumpNetwork();
    if (gMpIsHost) {
        mpCombatDrainQueue();
        mpCombatDrainEndRequest();
        // Co-op cheats: refill after every spend/drain this pump so the state
        // broadcast below carries the refilled values. MpTick does not run in
        // the blocking combat loops, so this is the only per-frame hook there.
        MpDebugCheatsTick();
        MpBroadcastObjectStates();
        MpBroadcastPlayerStates();
        // Mark the broadcast done for this tick so the main loop's MpTick
        // does not immediately repeat the (expensive) object-state sweep.
        MpNoteHostBroadcastTick();
    } else {
        mpCombatDrainEndRequest();
        MpDebugCheatsTick();
    }

    // The blocking combat loops (the host's own turn in _combat_input, NPC
    // turns in _combat_turn_run) never run MpCombatTick, which owns the
    // throttled card refresh — without one here the HUD player cards stay
    // empty black boxes for the whole host turn. Same throttle variable, so
    // this is just an additional call site.
    uint32_t now = getTicks();
    if (gMpCombat.inCombat && now - gCombatLastCardUpdate >= 500) {
        gCombatLastCardUpdate = now;
        mpCombatUpdateCards();
    }
}

// ---------------------------------------------------------------------------
// packet callbacks (host side)
// ---------------------------------------------------------------------------

void MpCombatOnStartRequest(Object* attacker, uint32_t targetNetId)
{
    if (!gMpIsHost || !gMpActive) {
        return;
    }
    if (gMpCombat.inCombat || isInCombat()) {
        MpLogAlways(MP_LOG_COMBAT, "start request rejected (already in combat)");
        return;
    }
    if (mpCombatVoteOrTransitionActive()) {
        MpLogAlways(MP_LOG_COMBAT, "start request rejected (vote active)");
        return;
    }
    gMpCombat.pendingStart = true;
    gPendingStartAttacker = attacker;
    gPendingStartTargetNetId = targetNetId;
    MpLog(MP_LOG_COMBAT, "start request queued attacker=%p targetNet=%u",
        (void*)attacker, targetNetId);
}

void MpCombatOnCmd(uint8_t netId, const NetCombatCmdPayload* payload)
{
    if (!gMpIsHost || !gMpActive || !gMpCombat.inCombat) {
        return;
    }
    if (netId == 0 || netId > NET_MAX_PLAYERS) {
        return;
    }
    MpCombatQueuedCmd queued;
    queued.netId = netId;
    queued.payload = *payload;
    gCombatCmdQueue.push_back(queued);
    MpLog(MP_LOG_COMBAT, "cmd queued netId=%u cmd=%d", netId, payload->cmd);
}

void MpCombatOnTurnEnd(uint8_t netId)
{
    if (!gMpIsHost || !gMpActive || !gMpCombat.inCombat) {
        return;
    }
    if (gMpCombat.waitingForTurnEnd == netId) {
        // Do NOT clear the wait yet: the player's final actions (attack
        // intents sent just before the turn end) may still be in the cmd
        // queue and must drain while the wait state is still theirs. The
        // wait loop breaks on turnEndPending after the drain.
        gMpCombat.turnEndPending = true;
        MpLog(MP_LOG_COMBAT, "turn end received netId=%u", netId);
    } else {
        MpLogAlways(MP_LOG_COMBAT, "turn end ignored netId=%u waiting=%u", netId, gMpCombat.waitingForTurnEnd);
    }
}

void MpCombatOnEndRequest(uint8_t netId)
{
    if (!gMpIsHost || !gMpActive || !gMpCombat.inCombat) {
        return;
    }
    gMpCombat.endRequestPending = true;
    gMpCombat.endRequesterNetId = netId;
    MpLog(MP_LOG_COMBAT, "end request received netId=%u", netId);
}

// ---------------------------------------------------------------------------
// packet callbacks (client side)
// ---------------------------------------------------------------------------

void MpCombatOnStartedPacket()
{
    if (!gMpIsClient || !gMpActive) {
        return;
    }
    if (gMpCombat.inCombat) {
        MpLogAlways(MP_LOG_COMBAT, "started ignored (already in mirror)");
        return;
    }
    gMpCombat.inCombat = true;
    gMpCombat.whoseTurn = 0;
    gMpCombat.startRequestPending = false;
    // Mirror hygiene: never inherit a stale EXIT_REQUESTED (the client turn
    // loop would insta-break and send TURN_END on the very first turn).
    gCombatState &= ~COMBAT_STATE_EXIT_REQUESTED;
    gCombatState |= COMBAT_STATE_IN_COMBAT;
    // Mirror the host's _combat_begin, which runs animationStop() the moment
    // combat starts and freezes every critter mid-stride. Stop the client's
    // local animations too — otherwise the dude keeps walking locally while
    // the host's avatar is already frozen, and the authoritative state
    // update snaps him hard back to the combat-start tile. Safe here: no
    // combat-flagged sequence can exist yet (isInCombat was false until this
    // packet), so _combat_turn_running cannot go negative.
    animationStop();
    keyboardReset();
    inputEventQueueReset();
    // allowScrolling=1: the wait cursor must not lock the camera — the player
    // pans the battlefield while watching the host's turn (vanilla _combat_begin
    // re-enables scrolling right after its own disable).
    gameUiDisable(1);
    gameMouseSetCursor(MOUSE_CURSOR_WAIT_WATCH);
    interfaceBarEndButtonsShow(true);
    // Render the AP panel in the vanilla wait posture (full red = no action
    // points available) instead of the live synced value: the mirrored AP
    // rides the round-start refresh and would paint a full GREEN bar while
    // the player simply waits (the b11 render gate then keeps it green until
    // his own turn). The red wait look is what vanilla shows a spectator;
    // MpCombatClientTurnLoop paints the real bar at his TURN_START.
    interfaceRenderActionPoints(-1, -1);
    mpCombatCreateCards();
    MpLog(MP_LOG_COMBAT, "client mirror entered");
}

// Host: an NPC's turn began. The acting-critter red outline is drawn by the
// vanilla host-side turn loop; the client never runs it, so mirror the turn
// via TURN_START (netId=0 carries the acting NPC).
void MpCombatOnNpcTurnStarted(Object* npc)
{
    if (!gMpIsHost || !gMpActive || npc == nullptr) {
        return;
    }
    NetCombatTurnStartPayload payload;
    payload.netId = 0;
    payload.ap = 0;
    payload.maxAp = 0;
    payload.targetNetId = MpGetObjNetId(npc);
    mpCombatBroadcast(NET_PKT_COMBAT_TURN_START, &payload, sizeof(payload));
    MpLog(MP_LOG_COMBAT, "npc turn start broadcast targetNet=%u", payload.targetNetId);
}

static void mpCombatClearActingNpcOutline()
{
    if (gMpClientActingNpcNetId == 0) {
        return;
    }
    Object* npc = MpFindObjByNetId(gMpClientActingNpcNetId);
    gMpClientActingNpcNetId = 0;
    if (npc != nullptr) {
        Rect rect;
        if (objectClearOutline(npc, &rect) == 0) {
            tileWindowRefreshRect(&rect, npc->elevation);
        }
    }
}

static void mpCombatSetActingNpcOutline(uint32_t targetNetId)
{
    mpCombatClearActingNpcOutline();
    if (targetNetId == 0) {
        return;
    }
    Object* npc = MpFindObjByNetId(targetNetId);
    if (npc == nullptr || critterIsDead(npc)) {
        return;
    }
    gMpClientActingNpcNetId = targetNetId;
    // Vanilla sets the acting critter's outline TYPE at combat begin via
    // _combat_update_critter_outline_for_los; the client mirror never runs
    // that (combat is host-authoritative), so every client critter has
    // outline == 0 and objectEnableOutline alone cannot render anything.
    // Set the type explicitly — team-based like vanilla — then enable it.
    int outlineType = gDude->data.critter.combat.team == npc->data.critter.combat.team
        ? OUTLINE_TYPE_FRIENDLY
        : OUTLINE_TYPE_HOSTILE;
    Rect rect;
    objectClearOutline(npc, nullptr);
    if (objectSetOutline(npc, outlineType, &rect) == 0) {
        objectEnableOutline(npc, nullptr);
        tileWindowRefreshRect(&rect, npc->elevation);
    }
}

void MpCombatOnTurnStart(const NetCombatTurnStartPayload* payload)
{
    if (!gMpIsClient || !gMpActive || !gMpCombat.inCombat) {
        return;
    }
    gMpCombat.whoseTurn = payload->netId;
    gMpCombat.turnAp = payload->ap;
    gMpCombat.turnMaxAp = payload->maxAp;
    if (payload->netId == 0) {
        // NPC turn: mirror the vanilla acting-critter red outline. The next
        // TURN_START (player or NPC) clears it.
        gMpCombat.turnStartPending = false;
        mpCombatSetActingNpcOutline(payload->targetNetId);
        MpLog(MP_LOG_COMBAT, "npc turn start targetNet=%u", payload->targetNetId);
        return;
    }
    // Player turn: any acting-NPC outline ends here.
    mpCombatClearActingNpcOutline();
    gMpCombat.turnStartPending = true;
    MpLog(MP_LOG_COMBAT, "turn start received netId=%u ap=%d maxAp=%d",
        payload->netId, payload->ap, payload->maxAp);
}

void MpCombatOnEndDenied()
{
    if (!gMpIsClient || !gMpActive) {
        return;
    }
    combatShowEndDeniedMessage();
    MpLogAlways(MP_LOG_COMBAT, "end denied received");
}

void MpCombatOnEndedPacket()
{
    if (!gMpIsClient || !gMpActive) {
        return;
    }
    MpCombatForceExit();
    MpLog(MP_LOG_COMBAT, "client mirror exited (ENDED)");
    MpCombatClearMoveIntent();
}

// ---------------------------------------------------------------------------
// client turn loop
// ---------------------------------------------------------------------------

int MpCombatClientTurnLoop()
{
    Object* dude = gDude;
    if (dude == nullptr) {
        MpLogAlways(MP_LOG_COMBAT, "client turn loop rejected no dude");
        return -1;
    }
    MpLog(MP_LOG_COMBAT, "client turn begin ap=%d maxAp=%d", gMpCombat.turnAp, gMpCombat.turnMaxAp);

    gMpCombat.turnStartPending = false;
    gMpCombat.turnActive = true;

    keyboardReset();
    inputEventQueueReset();
    _combat_free_move = 2 * perkGetRank(dude, PERK_BONUS_MOVE);
    interfaceRenderActionPoints(gMpCombat.turnAp, _combat_free_move);
    // Seed the local AP mirror from the AUTHORITATIVE turn-start AP. The
    // mirror is otherwise fed by the state-sync channel, which lags the host
    // by the command-queue round trip — a stale sync could show 0 AP and
    // soft-exit the loop (sending TURN_END) while the host still counts this
    // as the player's turn; every attack click then dies silently in the
    // vanilla not-player-turn gate. Seeding makes the exit fire on real
    // spends; later sync corrections only nudge the value.
    dude->data.critter.combat.ap = gMpCombat.turnAp;
    gameUiEnable();
    // The mirror keeps the wait cursor from combat begin; the player's own
    // turn must show the normal move cursor or clicks feel dead. The vanilla
    // dude-turn runs _gmouse_3d_refresh() here — without it the 3D cursor
    // objects (hex highlight) stay hidden from the wait posture and the
    // right-click mode cycle is disabled (vanilla gate), stranding the
    // player in whatever cursor mode the previous turn left behind.
    _gmouse_3d_refresh();
    gameMouseSetCursor(MOUSE_CURSOR_ARROW);
    gameMouseSetMode(GAME_MOUSE_MODE_MOVE);
    gCombatState |= COMBAT_STATE_PLAYER_TURN;
    interfaceBarEndButtonsRenderGreenLights();

    int rc = 0;

    while ((gCombatState & COMBAT_STATE_PLAYER_TURN) != 0) {
        MpCombatPump();
        mpCombatUpdateCards();

        if (!gMpCombat.inCombat) {
            rc = -1;
            break;
        }
        if ((gCombatState & COMBAT_STATE_EXIT_REQUESTED) != 0) {
            rc = -1;
            break;
        }
        if (_game_user_wants_to_quit != GAME_QUIT_REQUEST_NONE) {
            break;
        }
        if ((dude->data.critter.combat.results & (DAM_DEAD | DAM_KNOCKED_OUT | DAM_LOSE_TURN)) != 0) {
            break;
        }

        int key = inputGetInput();
        if (key == KEY_SPACE) {
            break; // end turn
        }
        if (key == KEY_RETURN) {
            if (!gMpCombat.endRequestPending) {
                gMpCombat.endRequestPending = true;
                gMpCombat.endRequesterNetId = gMpSession.localNetId;
                mpCombatSendNonEmpty(NET_PKT_COMBAT_END_REQUEST);
                MpLog(MP_LOG_COMBAT, "end request sent");
            }
            continue;
        }
        if (key != -1) {
            _scripts_check_state_in_combat();
            gameHandleKey(key, true);
        }

        int ap = dude->data.critter.combat.ap;
        if (ap <= 0 && _combat_free_move <= 0) {
            // Soft local exit only: the turn's real end is host-owned — this
            // break merely sends TURN_END, and the host validates it against
            // its own authoritative AP. With thin-client AP (no local spends)
            // the mirrored value lags the host by at most one round trip, so
            // the exit can fire a beat late; the host never double-ends.
            break;
        }

        renderPresent();
        sharedFpsLimiter.mark();
        sharedFpsLimiter.throttle();
    }

    gCombatState &= ~COMBAT_STATE_PLAYER_TURN;
    gMpCombat.turnActive = false;

    if (gMpCombat.inCombat) {
        // Turn over, combat still on: fall into the wait posture until the
        // next TURN_START arrives. Scrolling stays enabled so the player can
        // pan the camera while waiting.
        gameUiDisable(1);
        gameMouseSetCursor(MOUSE_CURSOR_WAIT_WATCH);
        interfaceBarEndButtonsRenderRedLights();
        interfaceRenderActionPoints(-1, -1);
        mpCombatSendNonEmpty(NET_PKT_COMBAT_TURN_END);
        MpLog(MP_LOG_COMBAT, "turn end sent");
    } else {
        // Combat ended while this turn was running: COMBAT_ENDED already ran
        // MpCombatForceExit and restored the UI. Re-applying the wait posture
        // here would overwrite that restore and strand the player on a
        // disabled UI with the wait cursor forever.
        MpLog(MP_LOG_COMBAT, "client turn end after mirror exit, wait posture skipped");
    }

    MpLog(MP_LOG_COMBAT, "client turn end rc=%d", rc);
    return rc;
}

void MpCombatSendStartRequest(Object* defender)
{
    if (!gMpIsClient || !gMpActive) {
        return;
    }
    if (gMpCombat.inCombat || isInCombat()) {
        MpLog(MP_LOG_COMBAT, "start request suppressed (already in combat)");
        return;
    }
    if (gMpCombat.startRequestPending) {
        MpLog(MP_LOG_COMBAT, "start request suppressed (already pending)");
        return;
    }
    gMpCombat.startRequestPending = true;
    NetCombatStartRequestPayload payload;
    payload.targetNetId = defender != nullptr ? MpGetObjNetId(defender) : 0;
    mpCombatSend(NET_PKT_COMBAT_START_REQUEST, &payload, sizeof(payload));
    MpLog(MP_LOG_COMBAT, "start request sent targetNet=%u", payload.targetNetId);
}

void MpCombatSendEndRequest()
{
    if (!gMpActive || !MpCombatIsActive()) {
        return;
    }
    if (gMpCombat.endRequestPending) {
        return;
    }
    gMpCombat.endRequestPending = true;
    gMpCombat.endRequesterNetId = gMpSession.localNetId;
    if (gMpIsHost) {
        // The host validates locally; the drain runs from MpTick or a pump.
        MpLog(MP_LOG_COMBAT, "end request queued (host)");
    } else {
        mpCombatSendNonEmpty(NET_PKT_COMBAT_END_REQUEST);
        MpLog(MP_LOG_COMBAT, "end request sent");
    }
}

// The client's last clicked move destination (one intent per click). The host
// echoes every move resolution; a resolution equal to this tile means the
// move was accepted as-is and the local optimistic walk may finish.
static int gMpLastMoveIntentTile = -1;
static int gMpLastMoveIntentElevation = -1;

bool MpCombatHasPendingMoveIntent(int* outIntentTile)
{
    if (outIntentTile != nullptr) {
        *outIntentTile = gMpLastMoveIntentTile;
    }
    return gMpLastMoveIntentTile >= 0;
}

void MpCombatClearMoveIntent()
{
    gMpLastMoveIntentTile = -1;
    gMpLastMoveIntentElevation = -1;
}

void MpCombatSendMoveIntent(int tile, int elevation, bool isRun)
{
    if (!gMpIsClient || !gMpActive || !gMpCombat.inCombat || !gMpCombat.turnActive) {
        return;
    }
    NetCombatCmdPayload payload;
    memset(&payload, 0, sizeof(payload));
    payload.cmd = NET_COMBAT_CMD_MOVE;
    payload.reserved = isRun ? 0x01 : 0x00;
    payload.tile = tile;
    payload.elevation = elevation;
    mpCombatSend(NET_PKT_COMBAT_CMD, &payload, sizeof(payload));
    gMpLastMoveIntentTile = tile;
    gMpLastMoveIntentElevation = elevation;
    MpLog(MP_LOG_COMBAT, "move intent sent tile=%d elev=%d run=%d", tile, elevation, isRun ? 1 : 0);

    // Client-side free-move prediction: the host consumes the avatar's Bonus
    // Move points first (one per resolved tile), then real AP. Mirror that
    // locally so the golden bar shrinks live and the turn's soft exit fires
    // when both pools are empty. The real AP itself is tick-owned — the
    // state sync carries the authoritative value; only the free pool (which
    // is not on the wire) needs this local mirror.
    if (_combat_free_move > 0) {
        _combat_free_move--;
        interfaceRenderActionPoints(
            gDude != nullptr ? gDude->data.critter.combat.ap : 0, _combat_free_move);
    }
}

void MpCombatSendInventoryApCost(int cost)
{
    if (!gMpIsClient || !gMpActive || gMpSession.hostPeer == nullptr) {
        return;
    }
    // Generic player-command route — the host applies it to our avatar and
    // the state channel carries the result back. Not combat-gated: it is a
    // plain runtime-field command.
    NetPlayerCmdPayload payload;
    payload.opcode = NET_PLAYER_CMD_INVENTORY_AP;
    payload.arg1 = cost;
    payload.arg2 = 0;
    NetSendPacket(gMpSession.hostPeer, NET_CHANNEL_RELIABLE,
        NET_PKT_PLAYER_CMD, &payload, sizeof(payload));
    MpLog(MP_LOG_COMBAT, "inv ap cost sent cost=%d", cost);
}

void MpCombatSendAttackIntent(Object* target, HitMode hitMode, HitLocation hitLocation)
{
    if (!gMpIsClient || !gMpActive || !gMpCombat.inCombat || !gMpCombat.turnActive) {
        return;
    }
    uint32_t targetNetId = target != nullptr ? MpGetObjNetId(target) : 0;
    if (targetNetId == 0) {
        // Diagnostic: identify the unregistered target (pid/fid distinguish a
        // profile runtime avatar from a map-synced duplicate) and whether the
        // reverse map holds it under any id.
        int inMap = 0;
        if (target != nullptr && gMpSession.netIdToObj != nullptr) {
            for (int i = 1; i < gMpSession.netIdToObjCount; i++) {
                if (gMpSession.netIdToObj[i] == target) {
                    inMap = i;
                    break;
                }
            }
        }
        MpLogAlways(MP_LOG_COMBAT, "attack intent dropped target=%p pid=0x%X fid=0x%X tile=%d elev=%d isDude=%d count=%d inMapAt=%d",
            (void*)target, target != nullptr ? target->pid : 0,
            target != nullptr ? target->fid : 0,
            target != nullptr ? target->tile : -1,
            target != nullptr ? target->elevation : -1,
            target == gDude ? 1 : 0,
            gMpSession.netIdToObjCount, inMap);
        return;
    }
    NetCombatCmdPayload payload;
    memset(&payload, 0, sizeof(payload));
    payload.cmd = NET_COMBAT_CMD_ATTACK;
    payload.hitMode = (uint16_t)hitMode;
    payload.hitLocation = (uint16_t)hitLocation;
    payload.targetNetId = targetNetId;
    mpCombatSend(NET_PKT_COMBAT_CMD, &payload, sizeof(payload));
    MpLog(MP_LOG_COMBAT, "attack intent sent targetNetId=%u mode=%d loc=%d",
        targetNetId, (int)hitMode, (int)hitLocation);
}

void MpCombatForceExit()
{
    if (!gMpActive) {
        return;
    }
    bool wasInMirror = gMpCombat.inCombat;
    // Any acting-NPC outline mirror must go with the mirror, or a dead critter
    // could keep a stale red outline forever.
    mpCombatClearActingNpcOutline();
    gMpCombat = {};
    if (wasInMirror) {
        keyboardReset();
        inputEventQueueReset();
        interfaceBarEndButtonsHide(true);
        interfaceBarEndButtonsRenderRedLights();
        // Mirror the vanilla _combat_over HUD restore so the bar returns to
        // its normal out-of-combat state (AP panel, mouse mode, armor class).
        gDude->data.critter.combat.ap = critterGetStat(gDude, STAT_MAXIMUM_ACTION_POINTS);
        gCombatState &= ~(COMBAT_STATE_IN_COMBAT | COMBAT_STATE_PLAYER_TURN | COMBAT_STATE_EXIT_REQUESTED);
        gCombatState |= COMBAT_STATE_PLAYER_TURN;
        // The vanilla combat end clears the critter outlines (_combat_over);
        // the mirror exits via this path instead, so clear them here or the
        // hover/combat highlights survive the ENDED packet.
        _combat_outline_off();
        interfaceRenderActionPoints(0, 0);
        gameUiEnable();
        // gameUiEnable restores the interface but never the 2D cursor, and
        // the mirror almost always ends on WAIT_WATCH. Restore the move
        // cursor or the player is stranded staring at the watch after combat.
        gameMouseSetCursor(MOUSE_CURSOR_ARROW);
        gameMouseSetMode(GAME_MOUSE_MODE_MOVE);
        interfaceRenderArmorClass(true);
        mpCombatDestroyCards();
    }
}

// ---------------------------------------------------------------------------
// display-monitor sync (host -> clients)
// ---------------------------------------------------------------------------

// Host: the display-monitor broadcast suppression window. Set around the
// display of the host's OWN first-person combat lines (mpCombatMonitorLine
// in combat.cc) so they render on the host's screen only — the clients would
// read "you" as themselves.
static bool gMpMonitorBroadcastSuppressed = false;

void MpCombatSetMonitorBroadcastSuppressed(bool suppress)
{
    gMpMonitorBroadcastSuppressed = suppress;
}

bool MpCombatMonitorBroadcastSuppressed()
{
    return gMpMonitorBroadcastSuppressed;
}

void MpCombatBroadcastMonitorMessage(const char* text)
{
    if (!gMpIsHost || !gMpActive || gMpSession.enetHost == nullptr || text == nullptr) {
        return;
    }
    char buffer[MP_COMBAT_MESSAGE_MAX_LENGTH + 1];
    strncpy(buffer, text, MP_COMBAT_MESSAGE_MAX_LENGTH);
    buffer[MP_COMBAT_MESSAGE_MAX_LENGTH] = '\0';
    size_t length = strlen(buffer) + 1; // include the NUL
    NetBroadcastPacket(gMpSession.enetHost, NET_CHANNEL_RELIABLE,
        NET_PKT_COMBAT_MESSAGE, buffer, length);
}

void MpCombatSendMonitorMessageToPlayer(uint32_t netId, const char* text)
{
    if (!gMpIsHost || !gMpActive || gMpSession.enetHost == nullptr || text == nullptr) {
        return;
    }
    if (netId == 0 || netId > NET_MAX_PLAYERS) {
        return;
    }
    MultiplayerPlayer* p = &gMpSession.players[netId - 1];
    if (p == nullptr || !p->isConnected || p->peer == nullptr) {
        return;
    }
    char buffer[MP_COMBAT_MESSAGE_MAX_LENGTH + 1];
    strncpy(buffer, text, MP_COMBAT_MESSAGE_MAX_LENGTH);
    buffer[MP_COMBAT_MESSAGE_MAX_LENGTH] = '\0';
    size_t length = strlen(buffer) + 1; // include the NUL
    NetSendPacket(p->peer, NET_CHANNEL_RELIABLE,
        NET_PKT_COMBAT_MESSAGE, buffer, length);
}

void MpCombatOnMonitorMessage(const char* text, size_t textLength)
{
    if (!gMpIsClient || !gMpActive || text == nullptr || textLength == 0) {
        return;
    }
    char buffer[MP_COMBAT_MESSAGE_MAX_LENGTH + 1];
    size_t copy = textLength > MP_COMBAT_MESSAGE_MAX_LENGTH
        ? MP_COMBAT_MESSAGE_MAX_LENGTH
        : textLength;
    memcpy(buffer, text, copy);
    buffer[copy] = '\0';
    // This line IS the authoritative host version — bypass the local
    // prediction suppression while applying it.
    bool savedSuppress = gMpCombat.suppressLocalMonitor;
    gMpCombat.suppressLocalMonitor = false;
    displayMonitorAddMessage(buffer);
    gMpCombat.suppressLocalMonitor = savedSuppress;
}

// Host -> acting client: authoritative outcome of the client's move intent.
// The client's local dude walked optimistically toward the clicked tile; a
// resolution elsewhere (reject, occupancy truncation) stops the walk and
// snaps, so the vanilla targeting UI reads the true geometry.
void MpCombatOnMoveResult(const NetCombatMoveResultPayload* payload)
{
    if (!gMpIsClient || !gMpActive || payload == nullptr
        || payload->netId != gMpSession.localNetId) {
        return;
    }
    Object* dude = gDude;
    if (dude == nullptr || !hexGridTileIsValid((int)payload->tile)
        || !elevationIsValid(payload->elevation)) {
        return;
    }
    // A resolution equal to our own clicked destination means the move was
    // accepted as-is — the local walk is still playing and must not be cut.
    if ((int)payload->tile == gMpLastMoveIntentTile
        && payload->elevation == gMpLastMoveIntentElevation) {
        return;
    }
    if (dude->tile == (int)payload->tile && dude->elevation == payload->elevation) {
        MpCombatClearMoveIntent(); // resolved elsewhere; nothing to snap
        return; // already there
    }
    if (animationIsBusy(dude) != 0) {
        reg_anim_clear(dude);
    }
    const int oldIntent = gMpLastMoveIntentTile;
    MpApplyLocalDudeSnap((int)payload->tile, payload->elevation);
    MpCombatClearMoveIntent();
    MpLog(MP_LOG_COMBAT, "move result snap tile=%d elev=%d (intent was %d)",
        (int)payload->tile, payload->elevation, oldIntent);
}

// Client: a relayed script float_msg (combat chatter). Scripts only run on
// the host, so the host mirrors the text over the same critter; mirror the
// host's opFloatMessage display rules (colors, font, screen centering).
// Dialogue float stack (client side): ordered owner list of the live
// dialogue floats. Every new NET_FLOAT_DIALOG_STYLE float pushes the older
// ones one float-height up so the newest sits at the bottom — mirrors the
// host's mpDialogFloat policy exactly.
static Object* gMpClientFloatStack[MP_DIALOG_FLOAT_STACK_MAX];
static int gMpClientFloatStackCount = 0;

void MpCombatOnFloatMessage(const NetFloatMessagePayload* payload)
{
    if (!gMpIsClient || !gMpActive || payload == nullptr || payload->text[0] == '\0') {
        return;
    }
    Object* obj = MpFindObjByNetId(payload->netId);
    if (obj == nullptr || obj->elevation != gElevation) {
        return;
    }
    int color = COLOR_LIGHT_YELLOW;
    int backgroundColor = COLOR_BLACK;
    int font = 101;
    if (payload->type == NET_FLOAT_AI_STYLE || payload->type == NET_FLOAT_DIALOG_STYLE) {
        // Engine AI path (combatai.msg weapon comments) and synchronized
        // dialogue floats (NPC lines, player choices): the explicit style
        // does not map to a FloatingMessageType — use it verbatim so host
        // and client render the identical text.
        font = payload->font;
        color = payload->color;
        backgroundColor = payload->outline;
    } else switch (payload->type) {
    case -2: // FLOATING_MESSAGE_TYPE_WARNING
        color = COLOR_RED;
        backgroundColor = COLOR_BLACK;
        font = 103;
        tileSetCenter(gDude->tile, TILE_SET_CENTER_REFRESH_WINDOW);
        break;
    case 0: // NORMAL
    case 8: // YELLOW
        color = COLOR_LIGHT_YELLOW;
        break;
    case 1: // BLACK
    case 5: // PURPLE
    case 10: // GREY
        color = COLOR_DARK_GREY_2;
        break;
    case 2: // RED
        color = COLOR_RED;
        break;
    case 3: // GREEN
        color = COLOR_GREEN;
        break;
    case 4: // BLUE
        color = COLOR_BLUE;
        break;
    case 6: // NEAR_WHITE
        color = COLOR_LIGHT_GREY;
        break;
    case 7: // LIGHT_RED
        color = COLOR_LIGHT_RED;
        break;
    case 9: // WHITE
        color = COLOR_WHITE;
        break;
    case 11: // DARK_GREY
        color = COLOR_DARK_GREY;
        break;
    case 12: // LIGHT_GREY
        color = COLOR_GREY_2;
        break;
    }
    Rect rect;
    int addResult;
    if (payload->type == NET_FLOAT_DIALOG_STYLE) {
        // Stack first, add last (mirrors the host's mpDialogFloat): the old
        // floats climb by a UNIFORM amount so the new line lands at the
        // bottom slot, never caught in its own shift, and the old floats keep
        // their relative spacing. Dialogue floats never replace each other —
        // old lines fade naturally.
        const int floatHeight = textObjectMeasure(const_cast<char*>(payload->text), payload->font, payload->outline);
        if (floatHeight > 0) {
            const int dy = textObjectsComputeStackShift(obj->tile, floatHeight);
            if (dy > 0) {
                for (int i = 0; i < gMpClientFloatStackCount; i++) {
                    textObjectsShiftVertically(gMpClientFloatStack[i], -dy);
                }
            }
        }
        for (int i = 0; i < gMpClientFloatStackCount; i++) {
            if (gMpClientFloatStack[i] == obj) {
                gMpClientFloatStackCount--;
                for (int j = i; j < gMpClientFloatStackCount; j++) {
                    gMpClientFloatStack[j] = gMpClientFloatStack[j + 1];
                }
                break;
            }
        }
        if (gMpClientFloatStackCount < MP_DIALOG_FLOAT_STACK_MAX) {
            gMpClientFloatStack[gMpClientFloatStackCount++] = obj;
        }
        addResult = textObjectAddNoReplace(obj, const_cast<char*>(payload->text), font, color, backgroundColor, &rect);
    } else {
        addResult = textObjectAdd(obj, const_cast<char*>(payload->text), font, color, backgroundColor, &rect);
    }
    if (addResult != -1) {
        tileWindowRefreshRect(&rect, obj->elevation);
    }
}

void MpCombatBeginLocalAttackPrediction()
{
    if (gMpIsClient && gMpActive) {
        gMpCombat.suppressLocalMonitor = true;
    }
}

void MpCombatEndLocalAttackPrediction()
{
    if (gMpIsClient && gMpActive) {
        gMpCombat.suppressLocalMonitor = false;
    }
}

bool MpCombatMonitorSuppressed()
{
    return gMpIsClient && gMpActive && gMpCombat.inCombat && gMpCombat.suppressLocalMonitor;
}

// ---------------------------------------------------------------------------
// per-tick (mirror side / deferred work)
// ---------------------------------------------------------------------------

bool MpCombatIsPlayerCritter(const Object* obj)
{
    if (obj == nullptr) {
        return false;
    }
    return obj == gDude || MpCombatGetCritterPlayerNetId(const_cast<Object*>(obj)) != 0;
}

// Vanilla enemy detection: critter scripts check tile_distance(dude_obj,
// self_obj) to decide when to request combat. The scripts only ever look at
// gDude, so on the host we answer the dude_obj opcode with the NEAREST
// player critter for non-player scripts — the vanilla proximity logic then
// runs unchanged with its own distances, and the enemy enters combat as the
// vanilla script attacker (so it is a real combatant, not a bystander).
Object* MpCombatGetNearestPlayerTo(const Object* obj)
{
    if (obj == nullptr || !gMpIsHost || !gMpActive) {
        return nullptr;
    }
    Object* nearest = nullptr;
    int bestDistance = INT32_MAX;
    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        MultiplayerPlayer* player = &gMpSession.players[index];
        if (player->isConnected && player->obj != nullptr
            && player->obj->elevation == obj->elevation
            && !critterIsDead(player->obj)) {
            int distance = tileDistanceBetween(obj->tile, player->obj->tile);
            if (distance < bestDistance) {
                bestDistance = distance;
                nearest = player->obj;
            }
        }
    }
    return nearest;
}

void MpCombatTick()
{
    if (!gMpActive) {
        return;
    }

    if (gMpIsHost) {
        // Deferred combat start (from COMBAT_START_REQUEST). The requesting
        // client attacked a target, so build a CombatStartData that mirrors
        // a vanilla scripted ambush: the ambushing player acts first, the
        // attacked critter second. _combat blocks for the whole fight, so a
        // stack csd is valid for its entire duration.
        if (gMpCombat.pendingStart) {
            gMpCombat.pendingStart = false;
            Object* attacker = gPendingStartAttacker;
            uint32_t targetNetId = gPendingStartTargetNetId;
            gPendingStartAttacker = nullptr;
            gPendingStartTargetNetId = 0;

            // The request was queued when combat was off, but a local start
            // (e.g. the host's own encounter spawn requesting combat) may
            // have won the race since. Re-entering _combat mid-fight rebuilds
            // the teams from a stale/incomplete csd and half the combatants
            // can fall out — never double-start.
            if (gMpCombat.inCombat || isInCombat()) {
                MpLogAlways(MP_LOG_COMBAT, "host deferred start skipped (combat already active)");
                return;
            }

            if (attacker != nullptr && MpCombatIsPlayerCritter(attacker)
                && !critterIsDead(attacker)) {
                CombatStartData csd;
                memset(&csd, 0, sizeof(csd));
                csd.attacker = attacker;
                Object* defender = MpFindObjByNetId(targetNetId);
                if (defender != nullptr && !critterIsDead(defender)) {
                    csd.defender = defender;
                }
                MpLog(MP_LOG_COMBAT, "host starting combat (client ambush) netId=%u target=%p",
                    MpCombatGetCritterPlayerNetId(attacker), (void*)csd.defender);
                _combat(&csd);
            } else {
                MpLog(MP_LOG_COMBAT, "host starting combat (no valid ambusher)");
                _combat(nullptr);
            }
            return;
        }
        // Deferred end requests when nobody's blocking loop is running.
        if (gMpCombat.endRequestPending && gMpCombat.inCombat) {
            mpCombatDrainEndRequest();
        }
    } else {
        // Deferred blocking turn (from COMBAT_TURN_START). Never run it while
        // the client's loot modal is open: MpLootLoopTick pumps MpTick inside
        // the loot window, so the turn loop would nest inside the modal and
        // the player would be stuck in the loot screen through the combat.
        // MpLootLoopTick closes the session when combat turns active, then the
        // next top-level tick runs the deferred turn here.
        if (gMpCombat.turnStartPending && gMpCombat.inCombat
            && gMpCombat.whoseTurn == gMpSession.localNetId
            && !MpLootSessionOpen()) {
            MpCombatClientTurnLoop();
        }
    }

    // Cards refresh (throttled).
    uint32_t now = getTicks();
    if (gMpCombat.inCombat && now - gCombatLastCardUpdate >= 500) {
        gCombatLastCardUpdate = now;
        mpCombatUpdateCards();
    }
}

} // namespace fallout
