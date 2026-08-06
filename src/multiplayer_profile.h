#ifndef FALLOUT_MULTIPLAYER_PROFILE_H_
#define FALLOUT_MULTIPLAYER_PROFILE_H_

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "obj_types.h"
#include "perk_defs.h"
#include "proto_types.h"
#include "skill_defs.h"
#include "stat_defs.h"
#include "trait_defs.h"

namespace fallout {

constexpr uint16_t MP_PROFILE_SCHEMA_VERSION = 1;
constexpr size_t MP_PROFILE_NAME_LENGTH = 32;
constexpr size_t MP_PROFILE_MAX_BYTES = 16 * 1024 * 1024;
constexpr size_t MP_PROFILE_MAX_INVENTORY_NODES = 4096;
constexpr size_t MP_PROFILE_MAX_INVENTORY_DEPTH = 32;

struct MpInventoryNode {
    uint32_t id = 0;
    int32_t pid = -1;
    int32_t fid = 0;
    int32_t frame = 0;
    int32_t rotation = 0;
    int32_t flags = 0;
    int32_t dataFlags = 0;
    int32_t quantity = 1;
    int32_t lightDistance = 0;
    int32_t lightIntensity = 0;
    int32_t weaponAmmoQuantity = 0;
    int32_t weaponAmmoTypePid = -1;
    int32_t ammoQuantity = 0;
    int32_t miscCharges = 0;
    int32_t keyCode = 0;
    std::vector<uint32_t> children;
};

struct MpModelFile {
    std::string path;
    std::vector<uint8_t> data;
};

// Portable, authoritative state for one player. This structure intentionally
// contains no Object*, Script*, inventory pointers, local object ids, or local
// prototype ids. Those are reconstructed independently by each process.
struct MpPlayerProfile {
    uint16_t schemaVersion = MP_PROFILE_SCHEMA_VERSION;
    uint32_t generation = 1;
    uint32_t modelHash = 0;
    int32_t localModelIndex = -1;
    char name[MP_PROFILE_NAME_LENGTH] = {};

    int32_t prototypeMessageId = -1;
    int32_t prototypeFlags = 0;
    int32_t prototypeExtendedFlags = 0;
    int32_t prototypeLightDistance = 0;
    int32_t prototypeLightIntensity = 0;
    int32_t prototypeHeadFid = -1;
    int32_t prototypeFid = 0;
    int32_t prototypeAiPacket = 0;
    int32_t prototypeTeam = 0;

    int32_t critterFlags = 0;
    int32_t baseStats[SAVEABLE_STAT_COUNT] = {};
    int32_t bonusStats[SAVEABLE_STAT_COUNT] = {};
    int32_t skills[SKILL_COUNT] = {};
    int32_t bodyType = BODY_TYPE_BIPED;
    int32_t experience = 0;
    int32_t killType = KILL_TYPE_MAN;
    int32_t damageType = DAMAGE_TYPE_NORMAL;

    int32_t pcStats[PC_STAT_COUNT] = {};
    int32_t taggedSkills[NUM_TAGGED_SKILLS] = {};
    int32_t selectedTraits[TRAITS_MAX_SELECTED_COUNT] = {};
    int32_t perkRanks[PERK_COUNT] = {};
    int32_t killCounts[KILL_TYPE_DEFAULT_COUNT] = {};
    int32_t skillUseTimes[SKILL_COUNT][3] = {};
    int32_t sneakWorking = 0;
    int32_t editorLastLevel = 0;
    int32_t editorHasFreePerk = 0;
    int32_t remainingCharacterPoints = 0;

    int32_t tile = -1;
    int32_t x = 0;
    int32_t y = 0;
    int32_t sx = 0;
    int32_t sy = 0;
    int32_t frame = 0;
    int32_t rotation = ROTATION_NE;
    int32_t fid = 0;
    int32_t flags = OBJECT_NO_REMOVE | OBJECT_NO_SAVE;
    int32_t elevation = 0;
    int32_t lightDistance = 0;
    int32_t lightIntensity = 0;
    int32_t hp = 0;
    int32_t radiation = 0;
    int32_t poison = 0;
    int32_t reaction = 0;
    int32_t combatManeuver = 0;
    int32_t combatAp = 0;
    int32_t combatResults = 0;
    int32_t combatDamageLastTurn = 0;
    int32_t combatAiPacket = 0;
    int32_t combatTeam = 0;
    uint32_t whoHitMeNetId = 0;

    char modelName[13] = {};
    std::vector<MpModelFile> modelFiles;
    std::vector<uint32_t> rootInventory;
    std::vector<MpInventoryNode> inventory;
};

struct MpPlayerRuntime {
    MpPlayerProfile profile;
    Object* object = nullptr;
    int syntheticPid = -1;
};

bool MpProfileCaptureLocal(MpPlayerProfile* profile);
// Local capture without model file bytes; used for per-tick change detection.
bool MpProfileCaptureLocalNoModel(MpPlayerProfile* profile);
bool MpProfileCaptureObject(const Object* object, MpPlayerProfile* profile);
// Like MpProfileCaptureObject but without model file bytes. Used by the host
// per-tick change detection: model files are re-transferred only when the
// model identity (name/hash) changes, never re-captured for comparisons.
bool MpProfileCaptureObjectNoModel(const Object* object, MpPlayerProfile* profile);
bool MpProfileValidate(const MpPlayerProfile& profile);
uint32_t MpProfileHash(const MpPlayerProfile& profile);
bool MpProfileSerialize(const MpPlayerProfile& profile, std::vector<uint8_t>* data);
bool MpProfileDeserialize(const void* data, size_t dataLength, MpPlayerProfile* profile);

MpPlayerRuntime* MpProfileCreateRuntime(uint8_t netId, const MpPlayerProfile& profile,
    int tile, int elevation, int rotation);
bool MpProfileBindLocal(uint8_t netId, const MpPlayerProfile& profile, Object* object);
// Replaces the stored profile of an existing runtime (host side, after a
// captured mutation) while preserving the live object and synthetic prototype.
void MpProfileUpdateRuntime(uint8_t netId, const MpPlayerProfile& profile);
// Applies a canonical profile onto the local gDude (name, stats, skills,
// tags, traits, perks, kill counts, sneak, editor state, HP/radiation/poison).
// applyPcStats=false preserves the local pc-stat block (XP/level owned by the
// local instance; host-granted combat XP is applied via pcAddExperience
// delta before this call instead).
bool MpProfileApplyLocal(const MpPlayerProfile& profile, bool applyPcStats = true);
MpPlayerRuntime* MpProfileGetRuntime(uint8_t netId);
MpPlayerRuntime* MpProfileFindRuntimeByObject(const Object* object);
void MpProfileDetachAvatar(uint8_t netId);
void MpProfileDestroyRuntime(uint8_t netId);
void MpProfileDestroyAllRuntimes();

// Host: a remote player uploaded an updated profile mid-session. Updates the
// runtime profile (model-aware) and re-applies the character sheet (proto
// stats, skills, flags, experience, inventory) onto the LIVE avatar object —
// never recreating it. Transform/HP/combat fields are ignored (they live on
// the per-tick state channel). Returns false when the avatar is not available.
bool MpProfileApplyRuntimeUpdate(uint8_t netId, const MpPlayerProfile& profile);

// Host: grant combat XP to every remote player's avatar. Only the avatar's
// proto experience is bumped — the stored runtime profile is left untouched
// so the per-tick change detection sees the delta and rebroadcasts the
// profile, and each client applies the XP through its own level-up path.
void MpProfileGrantCombatXp(int xp);

// Returns the canonical profile name for a network avatar, or nullptr for a
// normal engine object. The returned pointer remains owned by the profile.
const char* MpProfileGetName(const Object* object);
bool MpProfileIsNetworkPlayer(const Object* object);
int MpProfileGetPcStat(const Object* object, int pcStat);
int MpProfileSetPcStat(Object* object, int pcStat, int value);
bool MpProfileIsTagged(const Object* object, Skill skill);

} // namespace fallout

#endif /* FALLOUT_MULTIPLAYER_PROFILE_H_ */
