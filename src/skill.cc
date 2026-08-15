#include "skill.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <assert.h>

#include "actions.h"
#include "color.h"
#include "combat.h"
#include "critter.h"
#include "debug.h"
#include "display_monitor.h"
#include "game.h"
#include "interface.h"
#include "item.h"
#include "message.h"
#include "multiplayer.h"
#include "multiplayer_combat.h"
#include "multiplayer_debug.h"
#include "multiplayer_profile.h"
#include "object.h"
#include "palette.h"
#include "party_member.h"
#include "perk.h"
#include "pipboy.h"
#include "platform_compat.h"
#include "proto.h"
#include "random.h"
#include "scripts.h"
#include "settings.h"
#include "sfall_config.h"
#include "sfall_script_hooks.h"
#include "stat.h"
#include "trait.h"
#include "multiplayer_log.h"

namespace fallout {

#define SKILLS_MAX_USES_PER_DAY (3)
#define SKILLS_MAX_COST_LEVEL (512)
#define SKILLS_MIN_RAW_POINTS (-128)
#define SKILLS_MIN_VALUE (-999)

#define REPAIRABLE_DAMAGE_FLAGS_LENGTH (5)
#define HEALABLE_DAMAGE_FLAGS_LENGTH (5)

typedef struct SkillDescription {
    char* name;
    char* description;
    char* attributes;
    int frmId;
    int defaultValue;
    int statModifier;
    Stat stat1;
    Stat stat2;
    int baseValueMult;
    int experience;
    int gainXpFromSkillPenalty;
} SkillDescription;

static void _show_skill_use_messages(Object* obj, Skill skill, Object* target, int successCount, int skillBonus);
static int skillGetFreeUsageSlot(Object* critter, Skill skill);
static void skillsInitDefaults();
static void skillsLoadCustomConfig();
static void skillsLoadCustomCosts(Config* config, Skill skill, const char* key);
static void skillsLoadCustomFormula(Config* config, Skill skill, const char* key);
static int skillGetCost(Skill skill, int skillValue);
static int skill_use_slot_clear();

// Damage flags which can be repaired using "Repair" skill.
//
// 0x4AA2F0
static const int gRepairableDamageFlags[REPAIRABLE_DAMAGE_FLAGS_LENGTH] = {
    DAM_BLIND,
    DAM_CRIP_ARM_LEFT,
    DAM_CRIP_ARM_RIGHT,
    DAM_CRIP_LEG_RIGHT,
    DAM_CRIP_LEG_LEFT,
};

// Damage flags which can be healed using "Doctor" skill.
//
// 0x4AA304
static const int gHealableDamageFlags[HEALABLE_DAMAGE_FLAGS_LENGTH] = {
    DAM_BLIND,
    DAM_CRIP_ARM_LEFT,
    DAM_CRIP_ARM_RIGHT,
    DAM_CRIP_LEG_RIGHT,
    DAM_CRIP_LEG_LEFT,
};

// 0x51D118 skill_data
static SkillDescription gSkillDescriptions[SKILL_COUNT] = {
    { nullptr, nullptr, nullptr, 28, 5, 4, STAT_AGILITY, STAT_INVALID, 1, 0, 0 },
    { nullptr, nullptr, nullptr, 29, 0, 2, STAT_AGILITY, STAT_INVALID, 1, 0, 0 },
    { nullptr, nullptr, nullptr, 30, 0, 2, STAT_AGILITY, STAT_INVALID, 1, 0, 0 },
    { nullptr, nullptr, nullptr, 31, 30, 2, STAT_AGILITY, STAT_STRENGTH, 1, 0, 0 },
    { nullptr, nullptr, nullptr, 32, 20, 2, STAT_AGILITY, STAT_STRENGTH, 1, 0, 0 },
    { nullptr, nullptr, nullptr, 33, 0, 4, STAT_AGILITY, STAT_INVALID, 1, 0, 0 },
    { nullptr, nullptr, nullptr, 34, 0, 2, STAT_PERCEPTION, STAT_INTELLIGENCE, 1, 25, 0 },
    { nullptr, nullptr, nullptr, 35, 5, 1, STAT_PERCEPTION, STAT_INTELLIGENCE, 1, 50, 0 },
    { nullptr, nullptr, nullptr, 36, 5, 3, STAT_AGILITY, STAT_INVALID, 1, 0, 0 },
    { nullptr, nullptr, nullptr, 37, 10, 1, STAT_PERCEPTION, STAT_AGILITY, 1, 25, 1 },
    { nullptr, nullptr, nullptr, 38, 0, 3, STAT_AGILITY, STAT_INVALID, 1, 25, 1 },
    { nullptr, nullptr, nullptr, 39, 10, 1, STAT_PERCEPTION, STAT_AGILITY, 1, 25, 1 },
    { nullptr, nullptr, nullptr, 40, 0, 4, STAT_INTELLIGENCE, STAT_INVALID, 1, 0, 0 },
    { nullptr, nullptr, nullptr, 41, 0, 3, STAT_INTELLIGENCE, STAT_INVALID, 1, 0, 0 },
    { nullptr, nullptr, nullptr, 42, 0, 5, STAT_CHARISMA, STAT_INVALID, 1, 0, 0 },
    { nullptr, nullptr, nullptr, 43, 0, 4, STAT_CHARISMA, STAT_INVALID, 1, 0, 0 },
    { nullptr, nullptr, nullptr, 44, 0, 5, STAT_LUCK, STAT_INVALID, 1, 0, 0 },
    { nullptr, nullptr, nullptr, 45, 0, 2, STAT_ENDURANCE, STAT_INTELLIGENCE, 1, 100, 0 },
};

static const SkillDescription defaultSkillDescriptions[SKILL_COUNT] = {
    { nullptr, nullptr, nullptr, 28, 5, 4, STAT_AGILITY, STAT_INVALID, 1, 0, 0 },
    { nullptr, nullptr, nullptr, 29, 0, 2, STAT_AGILITY, STAT_INVALID, 1, 0, 0 },
    { nullptr, nullptr, nullptr, 30, 0, 2, STAT_AGILITY, STAT_INVALID, 1, 0, 0 },
    { nullptr, nullptr, nullptr, 31, 30, 2, STAT_AGILITY, STAT_STRENGTH, 1, 0, 0 },
    { nullptr, nullptr, nullptr, 32, 20, 2, STAT_AGILITY, STAT_STRENGTH, 1, 0, 0 },
    { nullptr, nullptr, nullptr, 33, 0, 4, STAT_AGILITY, STAT_INVALID, 1, 0, 0 },
    { nullptr, nullptr, nullptr, 34, 0, 2, STAT_PERCEPTION, STAT_INTELLIGENCE, 1, 25, 0 },
    { nullptr, nullptr, nullptr, 35, 5, 1, STAT_PERCEPTION, STAT_INTELLIGENCE, 1, 50, 0 },
    { nullptr, nullptr, nullptr, 36, 5, 3, STAT_AGILITY, STAT_INVALID, 1, 0, 0 },
    { nullptr, nullptr, nullptr, 37, 10, 1, STAT_PERCEPTION, STAT_AGILITY, 1, 25, 1 },
    { nullptr, nullptr, nullptr, 38, 0, 3, STAT_AGILITY, STAT_INVALID, 1, 25, 1 },
    { nullptr, nullptr, nullptr, 39, 10, 1, STAT_PERCEPTION, STAT_AGILITY, 1, 25, 1 },
    { nullptr, nullptr, nullptr, 40, 0, 4, STAT_INTELLIGENCE, STAT_INVALID, 1, 0, 0 },
    { nullptr, nullptr, nullptr, 41, 0, 3, STAT_INTELLIGENCE, STAT_INVALID, 1, 0, 0 },
    { nullptr, nullptr, nullptr, 42, 0, 5, STAT_CHARISMA, STAT_INVALID, 1, 0, 0 },
    { nullptr, nullptr, nullptr, 43, 0, 4, STAT_CHARISMA, STAT_INVALID, 1, 0, 0 },
    { nullptr, nullptr, nullptr, 44, 0, 5, STAT_LUCK, STAT_INVALID, 1, 0, 0 },
    { nullptr, nullptr, nullptr, 45, 0, 2, STAT_ENDURANCE, STAT_INTELLIGENCE, 1, 100, 0 },
};

static double skillStatMultipliers[SKILL_COUNT][PRIMARY_STAT_COUNT];
static int skillCosts[SKILL_COUNT][SKILLS_MAX_COST_LEVEL];
static int tagSkillBonus = 20;
static bool tagPerkAppliesInitialBonus = false;
static bool tagSkillsDoublePointBonusDisabled = false;
static bool skillCostsBasedOnPoints = false;

// 0x51D430 gIsSteal
int _gIsSteal = 0;

// Something about stealing, base value?
//
// 0x51D434 gStealCount
int _gStealCount = 0;

// 0x51D438 gStealSize
int _gStealSize = 0;

// 0x667F98 timesSkillUsed
static int _timesSkillUsed[SKILL_COUNT][SKILLS_MAX_USES_PER_DAY];

// 0x668070 tag_skill
static Skill gTaggedSkills[NUM_TAGGED_SKILLS];

// sfall's set_skill_max limit. Global scripts restore content-specific values
// after every game reset.
static int skillMaximum = 300;

// skill.msg
//
// 0x668080 skill_message_file
static MessageList gSkillsMessageList;

static void skillsInitDefaults()
{
    for (Skill skill = SKILL_FIRST; skill < SKILL_COUNT; skill++) {
        char* name = gSkillDescriptions[skill].name;
        char* description = gSkillDescriptions[skill].description;
        char* attributes = gSkillDescriptions[skill].attributes;

        gSkillDescriptions[skill] = defaultSkillDescriptions[skill];
        gSkillDescriptions[skill].name = name;
        gSkillDescriptions[skill].description = description;
        gSkillDescriptions[skill].attributes = attributes;

        for (Stat stat = STAT_FIRST; stat < PRIMARY_STAT_COUNT; stat++) {
            skillStatMultipliers[skill][stat] = 0.0;
        }

        SkillDescription* skillDescription = &(gSkillDescriptions[skill]);
        if (skillDescription->stat1 != STAT_INVALID) {
            skillStatMultipliers[skill][skillDescription->stat1] = skillDescription->statModifier;
        }

        if (skillDescription->stat2 != STAT_INVALID) {
            skillStatMultipliers[skill][skillDescription->stat2] = skillDescription->statModifier;
        }

        for (int level = 0; level < SKILLS_MAX_COST_LEVEL; level++) {
            skillCosts[skill][level] = skillsGetCost(level);
        }
    }

    tagSkillBonus = 20;
    tagPerkAppliesInitialBonus = false;
    tagSkillsDoublePointBonusDisabled = false;
    skillCostsBasedOnPoints = false;
}

static Stat skillStatFromConfigLetter(char ch)
{
    switch (ch) {
    case 's':
    case 'S':
        return STAT_STRENGTH;
    case 'p':
    case 'P':
        return STAT_PERCEPTION;
    case 'e':
    case 'E':
        return STAT_ENDURANCE;
    case 'c':
    case 'C':
        return STAT_CHARISMA;
    case 'i':
    case 'I':
        return STAT_INTELLIGENCE;
    case 'a':
    case 'A':
        return STAT_AGILITY;
    case 'l':
    case 'L':
        return STAT_LUCK;
    default:
        return STAT_INVALID;
    }
}

static void skillsLoadCustomConfig()
{
    char* skillsFile = nullptr;
    configGetString(&gSfallConfig, SFALL_CONFIG_MISC_KEY, SFALL_CONFIG_SKILLS_FILE_KEY, &skillsFile);
    if (skillsFile == nullptr || skillsFile[0] == '\0') {
        return;
    }

    ScopedConfig config { skillsFile, false };
    if (!config) {
        debugPrint("Skills config %s not found.\n", skillsFile);
        return;
    }

    int configuredTagSkillBonus = 0;
    if (configGetInt(config.get(), "Skills", "TagSkillBonus", &configuredTagSkillBonus) && configuredTagSkillBonus >= 0 && configuredTagSkillBonus <= 100) {
        tagSkillBonus = configuredTagSkillBonus;
    }

    int tagSkillMode = 0;
    configGetInt(config.get(), "Skills", "TagSkillMode", &tagSkillMode, 0);
    tagPerkAppliesInitialBonus = (tagSkillMode & 1) != 0;
    tagSkillsDoublePointBonusDisabled = (tagSkillMode & 2) != 0;

    int basedOnPoints = 0;
    configGetInt(config.get(), "Skills", "BasedOnPoints", &basedOnPoints, 0);
    skillCostsBasedOnPoints = basedOnPoints != 0;

    char key[32];
    for (Skill skill = SKILL_FIRST; skill < SKILL_COUNT; skill++) {
        snprintf(key, sizeof(key), "Skill%d", skill);
        skillsLoadCustomFormula(config.get(), skill, key);

        snprintf(key, sizeof(key), "SkillCost%d", skill);
        skillsLoadCustomCosts(config.get(), skill, key);

        snprintf(key, sizeof(key), "SkillBase%d", skill);
        configGetInt(config.get(), "Skills", key, &(gSkillDescriptions[skill].defaultValue), gSkillDescriptions[skill].defaultValue);

        int skillMulti = 0;
        snprintf(key, sizeof(key), "SkillMulti%d", skill);
        if (configGetInt(config.get(), "Skills", key, &skillMulti)) {
            if (skillMulti < 1) {
                skillMulti = 1;
            } else if (skillMulti > 10) {
                skillMulti = 10;
            }
            gSkillDescriptions[skill].baseValueMult = skillMulti;
        }

        snprintf(key, sizeof(key), "SkillImage%d", skill);
        configGetInt(config.get(), "Skills", key, &(gSkillDescriptions[skill].frmId), gSkillDescriptions[skill].frmId);
    }
}

static void skillsLoadCustomCosts(Config* config, Skill skill, const char* key)
{
    char* string = nullptr;
    if (!configGetString(config, "Skills", key, &string) || string == nullptr) {
        return;
    }

    char buffer[512];
    strncpy(buffer, string, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    int upto = 0;
    int price = 1;
    char* token = strtok(buffer, "|");
    while (token != nullptr && upto < SKILLS_MAX_COST_LEVEL) {
        if (token[0] != '\0') {
            int next = atoi(token);
            while (upto < next && upto < SKILLS_MAX_COST_LEVEL) {
                skillCosts[skill][upto++] = price;
            }
            price++;
        }
        token = strtok(nullptr, "|");
    }

    while (upto < SKILLS_MAX_COST_LEVEL) {
        skillCosts[skill][upto++] = price;
    }
}

static void skillsLoadCustomFormula(Config* config, Skill skill, const char* key)
{
    char* string = nullptr;
    if (!configGetString(config, "Skills", key, &string) || string == nullptr) {
        return;
    }

    for (Stat stat = STAT_FIRST; stat < PRIMARY_STAT_COUNT; stat++) {
        skillStatMultipliers[skill][stat] = 0.0;
    }

    gSkillDescriptions[skill].statModifier = 0;
    gSkillDescriptions[skill].stat1 = STAT_INVALID;
    gSkillDescriptions[skill].stat2 = STAT_INVALID;

    char buffer[64];
    strncpy(buffer, string, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    char* token = strtok(buffer, "|");
    while (token != nullptr) {
        if (strlen(token) >= 2) {
            Stat stat = skillStatFromConfigLetter(token[0]);
            if (stat != STAT_INVALID) {
                skillStatMultipliers[skill][stat] = atof(token + 1);
                if (gSkillDescriptions[skill].stat1 == STAT_INVALID) {
                    gSkillDescriptions[skill].stat1 = stat;
                } else if (gSkillDescriptions[skill].stat2 == STAT_INVALID) {
                    gSkillDescriptions[skill].stat2 = stat;
                }
            } else {
                debugPrint("Warning: Invalid SPECIAL stat '%c' in Skills config key %s.\n", token[0], key);
            }
        }
        token = strtok(nullptr, "|");
    }
}

// 0x4AA318
int skillsInit()
{
    skillsInitDefaults();

    if (!messageListInit(&gSkillsMessageList)) {
        return -1;
    }

    char path[COMPAT_MAX_PATH];
    snprintf(path, sizeof(path), "%s%s", asc_5186C8, "skill.msg");

    if (!messageListLoad(&gSkillsMessageList, path)) {
        return -1;
    }

    for (Skill skill = SKILL_FIRST; skill < SKILL_COUNT; skill++) {
        MessageListItem messageListItem;

        messageListItem.num = 100 + skill;
        if (messageListGetItem(&gSkillsMessageList, &messageListItem)) {
            gSkillDescriptions[skill].name = messageListItem.text;
        }

        messageListItem.num = 200 + skill;
        if (messageListGetItem(&gSkillsMessageList, &messageListItem)) {
            gSkillDescriptions[skill].description = messageListItem.text;
        }

        messageListItem.num = 300 + skill;
        if (messageListGetItem(&gSkillsMessageList, &messageListItem)) {
            gSkillDescriptions[skill].attributes = messageListItem.text;
        }
    }

    skillsLoadCustomConfig();

    for (int index = 0; index < NUM_TAGGED_SKILLS; index++) {
        gTaggedSkills[index] = SKILL_INVALID;
    }

    // NOTE: Uninline.
    skill_use_slot_clear();

    messageListRepositorySetStandardMessageList(STANDARD_MESSAGE_LIST_SKILL, &gSkillsMessageList);

    return 0;
}

// 0x4AA448
void skillsReset()
{
    skillMaximum = 300;

    for (int index = 0; index < NUM_TAGGED_SKILLS; index++) {
        gTaggedSkills[index] = SKILL_INVALID;
    }

    // NOTE: Uninline.
    skill_use_slot_clear();
}

// 0x4AA478
void skillsExit()
{
    messageListRepositorySetStandardMessageList(STANDARD_MESSAGE_LIST_SKILL, nullptr);
    messageListFree(&gSkillsMessageList);
}

// 0x4AA488
int skillsLoad(File* stream)
{
    return fileReadInt32EnumList<Skill>(stream, gTaggedSkills, NUM_TAGGED_SKILLS);
}

// 0x4AA4A8
int skillsSave(File* stream)
{
    return fileWriteInt32EnumList<Skill>(stream, gTaggedSkills, NUM_TAGGED_SKILLS);
}

// 0x4AA4C8
void protoCritterDataResetSkills(CritterProtoData* data)
{
    for (Skill skill = SKILL_FIRST; skill < SKILL_COUNT; skill++) {
        data->skills[skill] = 0;
    }
}

// 0x4AA4E4
void skillsSetTagged(Skill* skills, int count)
{
    for (int index = 0; index < count; index++) {
        gTaggedSkills[index] = skills[index];
    }
}

// 0x4AA508
void skillsGetTagged(Skill* skills, int count)
{
    for (int index = 0; index < count; index++) {
        skills[index] = gTaggedSkills[index];
    }
}

// Co-op: while set, skillsPerformStealing writes its would-be feedback
// message here instead of the local monitor (the host relays it to the
// performing player's client).
static char* gSkillRemoteMessageSink = nullptr;
static size_t gSkillRemoteMessageSinkSize = 0;

void skillSetRemoteMessageSink(char* out, size_t size)
{
    gSkillRemoteMessageSink = out;
    gSkillRemoteMessageSinkSize = size;
}

// 0x4AA52C
bool skillIsTagged(Skill skill)
{
    return skill == gTaggedSkills[0]
        || skill == gTaggedSkills[1]
        || skill == gTaggedSkills[2]
        || skill == gTaggedSkills[3];
}

// 0x4AA558
int skillGetValue(Object* critter, Skill skill)
{
    if (!skillIsValid(skill)) {
        return -5;
    }

    Proto* proto;
    if (protoGetProto(critter->pid, &proto) == -1) {
        debugPrint("\nError: Failed to get a proto in skillGetValue for critter %d with a pid %d!", critter->id, critter->pid);
        return -5;
    }

    int baseValue = proto->critter.data.skills[skill];
    int rawSkillPoints = baseValue;
    if (baseValue < 0) {
        baseValue = 0;
    }

    SkillDescription* skillDescription = &(gSkillDescriptions[skill]);

    double value = skillDescription->defaultValue + baseValue * skillDescription->baseValueMult;
    for (Stat stat = STAT_FIRST; stat < PRIMARY_STAT_COUNT; stat++) {
        value += critterGetStat(critter, stat) * skillStatMultipliers[skill][stat];
    }

    if (critter == gDude || MpProfileIsNetworkPlayer(critter)) {
        if (MpProfileIsTagged(critter, skill)) {
            if (!tagSkillsDoublePointBonusDisabled) {
                value += baseValue * skillDescription->baseValueMult;
            }

            if (tagPerkAppliesInitialBonus || !perkGetRank(critter, PERK_TAG)
                || !(MpProfileFindRuntimeByObject(critter) != nullptr
                    && MpProfileFindRuntimeByObject(critter)->profile.taggedSkills[3] == skill)) {
                value += tagSkillBonus;
            }
        }

        value += traitGetSkillModifierFor(critter, skill);
        value += perkGetSkillModifier(critter, skill);
        value += skillGetGameDifficultyModifier(skill);
    }

    if (rawSkillPoints < 0) {
        if (rawSkillPoints < SKILLS_MIN_RAW_POINTS) {
            rawSkillPoints = SKILLS_MIN_RAW_POINTS;
        }

        rawSkillPoints *= skillDescription->baseValueMult;
        if (MpProfileIsTagged(critter, skill)) {
            rawSkillPoints *= 2;
        }

        value += rawSkillPoints;
    }

    int integerValue = static_cast<int>(value);
    if (rawSkillPoints < 0 && integerValue < 0) {
        if (integerValue < SKILLS_MIN_VALUE) {
            integerValue = SKILLS_MIN_VALUE;
        }
    }

    if (integerValue > skillMaximum) {
        integerValue = skillMaximum;
    }

    return integerValue;
}

void skillSetMaximum(int maximum)
{
    // Match sfall: out-of-range values restore the vanilla ceiling.
    skillMaximum = maximum >= 0 && maximum <= 300 ? maximum : 300;
}

// 0x4AA654
int skillGetDefaultValue(Skill skill)
{
    return skillIsValid(skill) ? gSkillDescriptions[skill].defaultValue : -5;
}

// 0x4AA6BC
int skillAdd(Object* obj, Skill skill)
{
    if (obj != gDude && !MpProfileIsNetworkPlayer(obj)) {
        return -5;
    }

    if (!skillIsValid(skill)) {
        return -5;
    }

    int unspentSp = MpProfileGetPcStat(obj, PC_STAT_UNSPENT_SKILL_POINTS);
    if (unspentSp <= 0) {
        return -4;
    }

    int skillValue = skillGetValue(obj, skill);
    if (skillValue >= skillMaximum) {
        return -3;
    }

    Proto* proto;
    if (protoGetProto(obj->pid, &proto) == -1) {
        return -5;
    }

    // NOTE: Uninline.
    int costValue = skillValue;
    if (skillCostsBasedOnPoints) {
        costValue = proto->critter.data.skills[skill];
    }

    int requiredSp = skillGetCost(skill, costValue);

    if (unspentSp < requiredSp) {
        return -4;
    }

    int rc = MpProfileSetPcStat(obj, PC_STAT_UNSPENT_SKILL_POINTS, unspentSp - requiredSp);
    if (rc == 0) {
        proto->critter.data.skills[skill] += 1;
    }

    return rc;
}

// 0x4AA7F8
int skillAddForce(Object* obj, Skill skill)
{
    if (obj != gDude && !MpProfileIsNetworkPlayer(obj)) {
        return -5;
    }

    if (!skillIsValid(skill)) {
        return -5;
    }

    if (skillGetValue(obj, skill) >= skillMaximum) {
        return -3;
    }

    Proto* proto;
    if (protoGetProto(obj->pid, &proto) == -1) {
        return -5;
    }

    proto->critter.data.skills[skill] += 1;

    return 0;
}

// Returns the cost of raising skill value in skill points.
//
// 0x4AA87C
int skillsGetCost(int skillValue)
{
    if (skillValue >= 201) {
        return 6;
    } else if (skillValue >= 176) {
        return 5;
    } else if (skillValue >= 151) {
        return 4;
    } else if (skillValue >= 126) {
        return 3;
    } else if (skillValue >= 101) {
        return 2;
    } else {
        return 1;
    }
}

static int skillGetCost(Skill skill, int skillValue)
{
    if (!skillIsValid(skill)) {
        return skillsGetCost(skillValue);
    }

    int costIndex = skillValue;
    if (costIndex < 0) {
        costIndex = 0;
    } else if (costIndex >= SKILLS_MAX_COST_LEVEL) {
        costIndex = SKILLS_MAX_COST_LEVEL - 1;
    }

    return skillCosts[skill][costIndex];
}

// Decrements specified skill value by one, returning appropriate amount as
// unspent skill points.
//
// 0x4AA8C4
int skillSub(Object* critter, Skill skill)
{
    if (critter != gDude && !MpProfileIsNetworkPlayer(critter)) {
        return -5;
    }

    if (!skillIsValid(skill)) {
        return -5;
    }

    int unspentSp = MpProfileGetPcStat(critter, PC_STAT_UNSPENT_SKILL_POINTS);
    int skillValue = skillGetValue(critter, skill) - 1;

    Proto* proto;
    if (protoGetProto(critter->pid, &proto) == -1) {
        return -5;
    }

    if (proto->critter.data.skills[skill] <= 0) {
        return -2;
    }

    // NOTE: Uninline.
    int costValue;
    if (skillCostsBasedOnPoints) {
        costValue = proto->critter.data.skills[skill] - 1;
    } else {
        proto->critter.data.skills[skill] -= 1;
        costValue = skillGetValue(critter, skill);
        proto->critter.data.skills[skill] += 1;
    }

    int requiredSp = skillGetCost(skill, costValue);

    int newUnspentSp = unspentSp + requiredSp;
    int rc = MpProfileSetPcStat(critter, PC_STAT_UNSPENT_SKILL_POINTS, newUnspentSp);
    if (rc != 0) {
        return rc;
    }

    proto->critter.data.skills[skill] -= 1;

    if (MpProfileIsTagged(critter, skill)) {
        int oldSkillCost = skillsGetCost(skillValue);
        int newSkillCost = skillsGetCost(skillGetValue(critter, skill));
        if (oldSkillCost != newSkillCost) {
            rc = MpProfileSetPcStat(critter, PC_STAT_UNSPENT_SKILL_POINTS, newUnspentSp - 1);
            if (rc != 0) {
                return rc;
            }
        }
    }

    if (proto->critter.data.skills[skill] < 0) {
        proto->critter.data.skills[skill] = 0;
    }

    return 0;
}

// Decrements specified skill value by one.
//
// 0x4AAA34
int skillSubForce(Object* obj, Skill skill)
{
    Proto* proto;

    if (obj != gDude && !MpProfileIsNetworkPlayer(obj)) {
        return -5;
    }

    if (!skillIsValid(skill)) {
        return -5;
    }

    if (protoGetProto(obj->pid, &proto) == -1) {
        return -5;
    }

    if (proto->critter.data.skills[skill] <= 0) {
        return -2;
    }

    proto->critter.data.skills[skill] -= 1;

    return 0;
}

// 0x4AAAA4
int skillRoll(Object* critter, Skill skill, int modifier, int* howMuch)
{
    if (!skillIsValid(skill)) {
        return ROLL_FAILURE;
    }

    if (critter == gDude && skill != SKILL_STEAL) {
        Object* partyMember = partyMemberGetBestInSkill(skill);
        if (partyMember != nullptr) {
            if (partyMemberGetBestSkill(partyMember) == skill) {
                critter = partyMember;
            }
        }
    }

    int skillValue = skillGetValue(critter, skill);

    // Co-op: the +30 sneak bonus on steal rolls applies to every player.
    // Remote players' sneak state lives in their avatar's proto flags (the
    // SNEAK toggle mirrors it there); gDude uses the vanilla dude state.
    if (skill == SKILL_STEAL) {
        bool sneaking = false;
        if (critter == gDude) {
            sneaking = dudeHasState(DUDE_STATE_SNEAKING) && dudeIsSneaking();
        } else if (MpProfileIsNetworkPlayer(critter)) {
            MpPlayerRuntime* runtime = MpProfileFindRuntimeByObject(critter);
            if (runtime != nullptr) {
                sneaking = runtime->profile.sneakWorking != 0
                    && (runtime->profile.critterFlags & (1 << DUDE_STATE_SNEAKING)) != 0;
            } else {
                Proto* proto;
                if (protoGetProto(critter->pid, &proto) == 0) {
                    sneaking = (proto->critter.data.flags & (1 << DUDE_STATE_SNEAKING)) != 0;
                }
            }
        }
        if (sneaking) {
            skillValue += 30;
        }
    }

    if (MpDebugCheatEnabled(critter, MP_DEBUG_CHEAT_ALWAYS_SUCCEED)) {
        if (howMuch != nullptr) {
            *howMuch = 100;
        }
        MpLog(MP_LOG_STATS, "always succeed skill=%d netId=%u",
            skill, MpGetObjNetId(critter));
        return ROLL_SUCCESS;
    }

    int criticalChance = critterGetStat(critter, STAT_CRITICAL_CHANCE);
    return randomRoll(skillValue + modifier, criticalChance, howMuch);
}

// 0x4AAB9C
char* skillGetName(Skill skill)
{
    return skillIsValid(skill) ? gSkillDescriptions[skill].name : nullptr;
}

// 0x4AABC0
char* skillGetDescription(Skill skill)
{
    return skillIsValid(skill) ? gSkillDescriptions[skill].description : nullptr;
}

// 0x4AABE4
char* skillGetAttributes(Skill skill)
{
    return skillIsValid(skill) ? gSkillDescriptions[skill].attributes : nullptr;
}

// 0x4AAC08
int skillGetFrmId(Skill skill)
{
    return skillIsValid(skill) ? gSkillDescriptions[skill].frmId : 0;
}

// 0x4AAC2C
static void _show_skill_use_messages(Object* obj, Skill skill, Object* target, int successCount, int skillBonus)
{
    if (obj != gDude) {
        return;
    }

    if (successCount <= 0) {
        return;
    }

    SkillDescription* skillDescription = &(gSkillDescriptions[skill]);

    int baseExperience = skillDescription->experience;
    if (baseExperience == 0) {
        return;
    }

    if (skillDescription->gainXpFromSkillPenalty && skillBonus < 0) {
        baseExperience += abs(skillBonus);
    }

    int xpToAdd = successCount * baseExperience;

    int before = pcGetStat(PC_STAT_EXPERIENCE);

    if (pcAddExperience(xpToAdd) == 0 && successCount > 0) {
        MessageListItem messageListItem;
        messageListItem.num = 505; // You earn %d XP for honing your skills
        if (messageListGetItem(&gSkillsMessageList, &messageListItem)) {
            int after = pcGetStat(PC_STAT_EXPERIENCE);

            char text[60];
            snprintf(text, sizeof(text), messageListItem.text, after - before);
            displayMonitorAddMessage(text);
        }
    }
}

// Co-op: client-side formatting of a routed skill-use feedback message. The
// host sends the message id + args; the text comes from the shared message
// list and is formatted exactly like the vanilla display path.
bool skillGetMessageText(int messageId, char* dest, size_t size, int arg2, int arg3)
{
    MessageListItem item;
    item.num = messageId;
    if (!messageListGetItem(&gSkillsMessageList, &item)) {
        return false;
    }
    switch (messageId) {
    case 500: // You heal %d hit points.
    case 503: // You fail to do any healing.
        snprintf(dest, size, item.text, arg2);
        break;
    case 502: { // %s looks healthy already
        Object* target = MpFindObjByNetId((uint32_t)arg3);
        snprintf(dest, size, item.text,
            target != nullptr ? objectGetName(target) : "?");
        break;
    }
    case 520: // You heal your %s.
    case 521: // You heal the %s.
    case 525: // You fail to heal your %s.
    case 526: { // You fail to heal the %s.
        MessageListItem limb;
        limb.num = arg2;
        if (!messageListGetItem(&gSkillsMessageList, &limb)) {
            return false;
        }
        snprintf(dest, size, item.text, limb.text);
        break;
    }
    default: { // 501, 512-514, 590-592 — no placeholders
        if (!messageListGetItem(&gSkillsMessageList, &item)) {
            // Co-op: some routed messages (e.g. 902 combat-block) live in
            // proto.msg; fall back so the client can format them too.
            item.num = messageId;
            if (!messageListGetItem(&gProtoMessageList, &item)) {
                return false;
            }
        }
        strncpy(dest, item.text, size);
        dest[size - 1] = '\0';
        break;
    }
    }
    return true;
}

// Co-op: host-resolved skill use by a remote player. Vanilla feedback
// (monitor messages, the time-skip palette fade) would render on the HOST's
// screen — or nowhere, when gated on gDude — but must reach the performing
// player's client. Returns the performer's netId, or 0 for the host's own
// dude / singleplayer (vanilla behavior).
static uint8_t mpSkillRemoteNetId(const Object* obj)
{
    if (!gMpActive || !gMpIsHost || obj == nullptr || obj == gDude) {
        return 0;
    }
    return MpCombatGetCritterPlayerNetId(const_cast<Object*>(obj));
}

// Displays the message locally (host's own dude) or routes it to the
// performing player's client. |arg2|/|arg3| are the formatting args the
// client needs (healed HP, limb message id, target netId); |fade| makes the
// client play the time-skip blackout.
static void mpSkillFeedback(Object* obj, const MessageListItem* item,
    int arg2, int arg3, int fade)
{
    uint8_t netId = mpSkillRemoteNetId(obj);
    if (netId != 0) {
        MpSendSkillUseFeedback(netId, item->num, arg2, arg3, fade);
    } else {
        displayMonitorAddMessage(item->text);
    }
}

// skill_use
// 0x4AAD08
int skillUse(Object* obj, Object* target, Skill skill, int skillBonus)
{
    int hookResult = scriptHooks_UseSkill(obj, target, skill, skillBonus);
    if (hookResult != -1) {
        return hookResult;
    }

    MessageListItem messageListItem;
    char text[60];

    bool giveExp = true;
    int currentHp = critterGetStat(target, STAT_CURRENT_HIT_POINTS);
    int maximumHp = critterGetStat(target, STAT_MAXIMUM_HIT_POINTS);

    int hpToHeal = 0;
    int maximumHpToHeal = 0;
    int minimumHpToHeal = 0;

    // Co-op: every player is their own dude — remote players' heal amount
    // must scale with THEIR Healer perk, not the host's.
    if (obj == gDude || MpProfileIsNetworkPlayer(obj)) {
        if (skill == SKILL_FIRST_AID || skill == SKILL_DOCTOR) {
            int healerRank = perkGetRank(obj, PERK_HEALER);
            minimumHpToHeal = 4 * healerRank;
            maximumHpToHeal = 10 * healerRank;
        }
    }

    int skillOrCritSuccessBonus = critterGetStat(obj, STAT_CRITICAL_CHANCE) + skillBonus;

    int damageHealingAttempts = 1;
    int successCount = 0;
    bool skillUseSlotAdded = 0;

    switch (skill) {
    case SKILL_FIRST_AID:
        if (skillGetFreeUsageSlot(obj, SKILL_FIRST_AID) == -1) {
            // 590: You've taxed your ability with that skill. Wait a while.
            // 591: You're too tired.
            // 592: The strain might kill you.
            messageListItem.num = 590 + randomBetween(0, 2);
            if (messageListGetItem(&gSkillsMessageList, &messageListItem)) {
                mpSkillFeedback(obj, &messageListItem, 0, 0, 0);
            }

            return -1;
        }

        if (critterIsDead(target)) {
            if (gMpActive && gMpIsHost && MpCritterIsDownedPlayer(target)) {
                int roll = skillRoll(obj, skill, skillOrCritSuccessBonus, &hpToHeal);
                if (roll == ROLL_SUCCESS || roll == ROLL_CRITICAL_SUCCESS) {
                    hpToHeal = randomBetween(minimumHpToHeal + 1, maximumHpToHeal + 5);
                    MpReviveDownedPlayerWithHp(target, hpToHeal);
                    skillUpdateLastUse(obj, SKILL_FIRST_AID);
                    char text[256];
                    snprintf(text, sizeof(text), "%s revives %s with First Aid (+%d HP)!",
                        objectGetName(obj), objectGetName(target), hpToHeal);
                    displayMonitorAddMessage(text);
                    if (mpSkillRemoteNetId(obj) != 0) {
                        MpCombatSendMonitorMessageToPlayer(mpSkillRemoteNetId(obj), text);
                    }
                    uint8_t targetNetId = MpCombatGetCritterPlayerNetId(target);
                    if (targetNetId != 0 && targetNetId != mpSkillRemoteNetId(obj)) {
                        MpCombatSendMonitorMessageToPlayer(targetNetId, text);
                    }
                    return 0;
                } else {
                    messageListItem.num = 503;
                    if (messageListGetItem(&gSkillsMessageList, &messageListItem)) {
                        mpSkillFeedback(obj, &messageListItem, hpToHeal, 0, 1);
                    }
                    return -1;
                }
            } else {
                // 512: You can't heal the dead.
                // 513: Let the dead rest in peace.
                // 514: It's dead, get over it.
                messageListItem.num = 512 + randomBetween(0, 2);
                if (messageListGetItem(&gSkillsMessageList, &messageListItem)) {
                    debugPrint(messageListItem.text);
                }

                break;
            }
        }

        if (currentHp < maximumHp) {
            if (mpSkillRemoteNetId(obj) == 0) {
                paletteFadeTo(gPaletteBlack);
            }

            int roll;
            if (critterGetBodyType(target) == BODY_TYPE_ROBOTIC) {
                roll = ROLL_FAILURE;
            } else {
                roll = skillRoll(obj, skill, skillOrCritSuccessBonus, &hpToHeal);
            }

            if (roll == ROLL_SUCCESS || roll == ROLL_CRITICAL_SUCCESS) {
                hpToHeal = randomBetween(minimumHpToHeal + 1, maximumHpToHeal + 5);
                critterAdjustHitPoints(target, hpToHeal);

                if (obj == gDude) {
                    // You heal %d hit points.
                    messageListItem.num = 500;
                    if (!messageListGetItem(&gSkillsMessageList, &messageListItem)) {
                        return -1;
                    }

                    if (maximumHp - currentHp < hpToHeal) {
                        hpToHeal = maximumHp - currentHp;
                    }

                    snprintf(text, sizeof(text), messageListItem.text, hpToHeal);
                    displayMonitorAddMessage(text);
                } else if (mpSkillRemoteNetId(obj) != 0) {
                    MpSendSkillUseFeedback(mpSkillRemoteNetId(obj), 500, hpToHeal, 0, 1);
                }

                if (gMpActive && gMpIsHost && target != obj && target != gDude) {
                    uint8_t targetNetId = MpCombatGetCritterPlayerNetId(target);
                    if (targetNetId != 0 && targetNetId != mpSkillRemoteNetId(obj)) {
                        char targetNotify[256];
                        snprintf(targetNotify, sizeof(targetNotify), "%s healed you (+%d HP).", objectGetName(obj), hpToHeal);
                        MpCombatSendMonitorMessageToPlayer(targetNetId, targetNotify);
                    }
                }

                target->data.critter.combat.maneuver &= ~CRITTER_MANUEVER_FLEEING;

                skillUpdateLastUse(obj, SKILL_FIRST_AID);

                successCount = 1;

                if (target == gDude) {
                    interfaceRenderHitPoints(true);
                }
            } else {
                // You fail to do any healing.
                messageListItem.num = 503;
                if (!messageListGetItem(&gSkillsMessageList, &messageListItem)) {
                    return -1;
                }

                snprintf(text, sizeof(text), messageListItem.text, hpToHeal);
                mpSkillFeedback(obj, &messageListItem, hpToHeal, 0, 1);
            }

            scriptsExecMapUpdateProc();
            if (mpSkillRemoteNetId(obj) == 0) {
                paletteFadeTo(_cmap);
            }
        } else {
            if (obj == gDude) {
                // 501: You look healty already
                // 502: %s looks healthy already
                messageListItem.num = (target == gDude ? 501 : 502);
                if (!messageListGetItem(&gSkillsMessageList, &messageListItem)) {
                    return -1;
                }

                if (target == gDude) {
                    strcpy(text, messageListItem.text);
                } else {
                    snprintf(text, sizeof(text), messageListItem.text, objectGetName(target));
                }

                displayMonitorAddMessage(text);
                giveExp = false;
            } else if (mpSkillRemoteNetId(obj) != 0) {
                uint8_t perfNetId = mpSkillRemoteNetId(obj);
                MpSendSkillUseFeedback(perfNetId,
                    (target == gDude ? 501 : 502), 0,
                    target != gDude ? MpGetObjNetId(target) : 0, 0);
                giveExp = false;
            }
        }

        if (obj == gDude) {
            gameTimeAddSeconds(1800);
        }

        break;
    case SKILL_DOCTOR:
        if (skillGetFreeUsageSlot(obj, SKILL_DOCTOR) == -1) {
            // 590: You've taxed your ability with that skill. Wait a while.
            // 591: You're too tired.
            // 592: The strain might kill you.
            messageListItem.num = 590 + randomBetween(0, 2);
            if (messageListGetItem(&gSkillsMessageList, &messageListItem)) {
                mpSkillFeedback(obj, &messageListItem, 0, 0, 0);
            }

            return -1;
        }

        if (critterIsDead(target)) {
            if (gMpActive && gMpIsHost && MpCritterIsDownedPlayer(target)) {
                int roll = skillRoll(obj, skill, skillOrCritSuccessBonus, &hpToHeal);
                if (roll == ROLL_SUCCESS || roll == ROLL_CRITICAL_SUCCESS) {
                    hpToHeal = randomBetween(minimumHpToHeal + 4, maximumHpToHeal + 10);
                    MpReviveDownedPlayerWithHp(target, hpToHeal);
                    skillUpdateLastUse(obj, SKILL_DOCTOR);
                    Dam flags[HEALABLE_DAMAGE_FLAGS_LENGTH];
                    memcpy(flags, gHealableDamageFlags, sizeof(gHealableDamageFlags));
                    for (int index = 0; index < HEALABLE_DAMAGE_FLAGS_LENGTH; index++) {
                        target->data.critter.combat.results &= ~flags[index];
                    }
                    char text[256];
                    snprintf(text, sizeof(text), "%s revives %s with Doctor (+%d HP)!",
                        objectGetName(obj), objectGetName(target), hpToHeal);
                    displayMonitorAddMessage(text);
                    if (mpSkillRemoteNetId(obj) != 0) {
                        MpCombatSendMonitorMessageToPlayer(mpSkillRemoteNetId(obj), text);
                    }
                    uint8_t targetNetId = MpCombatGetCritterPlayerNetId(target);
                    if (targetNetId != 0 && targetNetId != mpSkillRemoteNetId(obj)) {
                        MpCombatSendMonitorMessageToPlayer(targetNetId, text);
                    }
                    return 0;
                } else {
                    messageListItem.num = 503;
                    if (messageListGetItem(&gSkillsMessageList, &messageListItem)) {
                        mpSkillFeedback(obj, &messageListItem, hpToHeal, 0, 1);
                    }
                    return -1;
                }
            } else {
                // 512: You can't heal the dead.
                // 513: Let the dead rest in peace.
                // 514: It's dead, get over it.
                messageListItem.num = 512 + randomBetween(0, 2);
                if (messageListGetItem(&gSkillsMessageList, &messageListItem)) {
                    mpSkillFeedback(obj, &messageListItem, 0, 0, 0);
                }
                break;
            }
        }

        if (currentHp < maximumHp || critterIsCrippled(target)) {
            if (mpSkillRemoteNetId(obj) == 0) {
                paletteFadeTo(gPaletteBlack);
            }

            if (critterGetBodyType(target) != BODY_TYPE_ROBOTIC && critterIsCrippled(target)) {
                Dam flags[HEALABLE_DAMAGE_FLAGS_LENGTH];
                memcpy(flags, gHealableDamageFlags, sizeof(gHealableDamageFlags));

                for (int index = 0; index < HEALABLE_DAMAGE_FLAGS_LENGTH; index++) {
                    if ((target->data.critter.combat.results & flags[index]) != 0) {
                        damageHealingAttempts++;

                        int roll = skillRoll(obj, skill, skillOrCritSuccessBonus, &hpToHeal);

                        // 530: damaged eye
                        // 531: crippled left arm
                        // 532: crippled right arm
                        // 533: crippled right leg
                        // 534: crippled left leg
                        messageListItem.num = 530 + index;
                        if (!messageListGetItem(&gSkillsMessageList, &messageListItem)) {
                            return -1;
                        }

                        MessageListItem prefix;

                        if (roll == ROLL_SUCCESS || roll == ROLL_CRITICAL_SUCCESS) {
                            target->data.critter.combat.results &= ~flags[index];
                            target->data.critter.combat.maneuver &= ~CRITTER_MANUEVER_FLEEING;

                            // 520: You heal your %s.
                            // 521: You heal the %s.
                            prefix.num = (target == gDude ? 520 : 521);

                            skillUpdateLastUse(obj, SKILL_DOCTOR);

                            successCount = 1;
                            skillUseSlotAdded = 1;

                            if (gMpActive && gMpIsHost && target != obj && target != gDude) {
                                uint8_t targetNetId = MpCombatGetCritterPlayerNetId(target);
                                if (targetNetId != 0 && targetNetId != mpSkillRemoteNetId(obj)) {
                                    char targetNotify[256];
                                    snprintf(targetNotify, sizeof(targetNotify), "%s healed your %s.", objectGetName(obj), messageListItem.text);
                                    MpCombatSendMonitorMessageToPlayer(targetNetId, targetNotify);
                                }
                            }
                        } else {
                            // 525: You fail to heal your %s.
                            // 526: You fail to heal the %s.
                            prefix.num = (target == gDude ? 525 : 526);
                        }

                        if (!messageListGetItem(&gSkillsMessageList, &prefix)) {
                            return -1;
                        }

                        snprintf(text, sizeof(text), prefix.text, messageListItem.text);
                        mpSkillFeedback(obj, &prefix, messageListItem.num, 0, 1);
                        _show_skill_use_messages(obj, skill, target, successCount, skillBonus);

                        giveExp = false;
                    }
                }
            }

            int roll;
            if (critterGetBodyType(target) == BODY_TYPE_ROBOTIC) {
                roll = ROLL_FAILURE;
            } else {
                int skillValue = skillGetValue(obj, skill);
                roll = randomRoll(skillValue, skillOrCritSuccessBonus, &hpToHeal);
            }

            if (roll == ROLL_SUCCESS || roll == ROLL_CRITICAL_SUCCESS) {
                hpToHeal = randomBetween(minimumHpToHeal + 4, maximumHpToHeal + 10);
                critterAdjustHitPoints(target, hpToHeal);

                if (obj == gDude) {
                    // You heal %d hit points.
                    messageListItem.num = 500;
                    if (!messageListGetItem(&gSkillsMessageList, &messageListItem)) {
                        return -1;
                    }

                    if (maximumHp - currentHp < hpToHeal) {
                        hpToHeal = maximumHp - currentHp;
                    }
                    snprintf(text, sizeof(text), messageListItem.text, hpToHeal);
                    displayMonitorAddMessage(text);
                } else if (mpSkillRemoteNetId(obj) != 0) {
                    MpSendSkillUseFeedback(mpSkillRemoteNetId(obj), 500, hpToHeal, 0, 1);
                }

                if (gMpActive && gMpIsHost && target != obj && target != gDude) {
                    uint8_t targetNetId = MpCombatGetCritterPlayerNetId(target);
                    if (targetNetId != 0 && targetNetId != mpSkillRemoteNetId(obj)) {
                        char targetNotify[256];
                        snprintf(targetNotify, sizeof(targetNotify), "%s healed you (+%d HP).", objectGetName(obj), hpToHeal);
                        MpCombatSendMonitorMessageToPlayer(targetNetId, targetNotify);
                    }
                }

                if (!skillUseSlotAdded) {
                    skillUpdateLastUse(obj, SKILL_DOCTOR);
                }

                target->data.critter.combat.maneuver &= ~CRITTER_MANUEVER_FLEEING;

                if (target == gDude) {
                    interfaceRenderHitPoints(true);
                }

                successCount = 1;
                _show_skill_use_messages(obj, skill, target, successCount, skillBonus);
                scriptsExecMapUpdateProc();
                if (mpSkillRemoteNetId(obj) == 0) {
                    paletteFadeTo(_cmap);
                }

                giveExp = false;
            } else {
                // You fail to do any healing.
                messageListItem.num = 503;
                if (!messageListGetItem(&gSkillsMessageList, &messageListItem)) {
                    return -1;
                }

                snprintf(text, sizeof(text), messageListItem.text, hpToHeal);
                mpSkillFeedback(obj, &messageListItem, hpToHeal, 0, 1);

                scriptsExecMapUpdateProc();
                if (mpSkillRemoteNetId(obj) == 0) {
                    paletteFadeTo(_cmap);
                }
            }
        } else {
            if (obj == gDude) {
                // 501: You look healty already
                // 502: %s looks healthy already
                messageListItem.num = (target == gDude ? 501 : 502);
                if (!messageListGetItem(&gSkillsMessageList, &messageListItem)) {
                    return -1;
                }

                if (target == gDude) {
                    strcpy(text, messageListItem.text);
                } else {
                    snprintf(text, sizeof(text), messageListItem.text, objectGetName(target));
                }

                displayMonitorAddMessage(text);

                giveExp = false;
            } else if (mpSkillRemoteNetId(obj) != 0) {
                uint8_t perfNetId = mpSkillRemoteNetId(obj);
                MpSendSkillUseFeedback(perfNetId,
                    (target == gDude ? 501 : 502), 0,
                    target != gDude ? MpGetObjNetId(target) : 0, 0);
                giveExp = false;
            }
        }

        if (obj == gDude) {
            gameTimeAddSeconds(3600 * damageHealingAttempts);
        }

        break;
    case SKILL_SNEAK:
    case SKILL_LOCKPICK:
        break;
    case SKILL_STEAL:
        scriptsRequestStealing(obj, target);
        break;
    case SKILL_TRAPS:
        messageListItem.num = 551; // You fail to find any traps.
        if (messageListGetItem(&gSkillsMessageList, &messageListItem)) {
            mpSkillFeedback(obj, &messageListItem, 0, 0, 0);
        }

        return -1;
    case SKILL_SCIENCE:
        messageListItem.num = 552; // You fail to learn anything.
        if (messageListGetItem(&gSkillsMessageList, &messageListItem)) {
            mpSkillFeedback(obj, &messageListItem, 0, 0, 0);
        }

        return -1;
    case SKILL_REPAIR:
        if (critterGetBodyType(target) != BODY_TYPE_ROBOTIC) {
            // You cannot repair that.
            messageListItem.num = 553;
            if (messageListGetItem(&gSkillsMessageList, &messageListItem)) {
                mpSkillFeedback(obj, &messageListItem, 0, 0, 0);
            }
            return -1;
        }

        if (skillGetFreeUsageSlot(obj, SKILL_REPAIR) == -1) {
            // 590: You've taxed your ability with that skill. Wait a while.
            // 591: You're too tired.
            // 592: The strain might kill you.
            messageListItem.num = 590 + randomBetween(0, 2);
            if (messageListGetItem(&gSkillsMessageList, &messageListItem)) {
                mpSkillFeedback(obj, &messageListItem, 0, 0, 0);
            }
            return -1;
        }

        if (critterIsDead(target)) {
            // The robotic unit is beyond repair.
            messageListItem.num = 601;
            if (messageListGetItem(&gSkillsMessageList, &messageListItem)) {
                mpSkillFeedback(obj, &messageListItem, 0, 0, 0);
            }
            break;
        }

        if (currentHp < maximumHp || critterIsCrippled(target)) {
            Dam flags[REPAIRABLE_DAMAGE_FLAGS_LENGTH];
            memcpy(flags, gRepairableDamageFlags, sizeof(gRepairableDamageFlags));

            if (mpSkillRemoteNetId(obj) == 0) {
                paletteFadeTo(gPaletteBlack);
            }

            for (int index = 0; index < REPAIRABLE_DAMAGE_FLAGS_LENGTH; index++) {
                if ((target->data.critter.combat.results & flags[index]) != 0) {
                    damageHealingAttempts++;

                    int roll = skillRoll(obj, skill, skillOrCritSuccessBonus, &hpToHeal);

                    // 530: damaged eye
                    // 531: crippled left arm
                    // 532: crippled right arm
                    // 533: crippled right leg
                    // 534: crippled left leg
                    messageListItem.num = 530 + index;
                    if (!messageListGetItem(&gSkillsMessageList, &messageListItem)) {
                        return -1;
                    }

                    MessageListItem prefix;

                    if (roll == ROLL_SUCCESS || roll == ROLL_CRITICAL_SUCCESS) {
                        target->data.critter.combat.results &= ~flags[index];
                        target->data.critter.combat.maneuver &= ~CRITTER_MANUEVER_FLEEING;

                        // 520: You heal your %s.
                        // 521: You heal the %s.
                        prefix.num = (target == gDude ? 520 : 521);
                        skillUpdateLastUse(obj, SKILL_REPAIR);

                        successCount = 1;
                        skillUseSlotAdded = 1;
                    } else {
                        // 525: You fail to heal your %s.
                        // 526: You fail to heal the %s.
                        prefix.num = (target == gDude ? 525 : 526);
                    }

                    if (!messageListGetItem(&gSkillsMessageList, &prefix)) {
                        return -1;
                    }

                    snprintf(text, sizeof(text), prefix.text, messageListItem.text);
                    mpSkillFeedback(obj, &prefix, messageListItem.num, 0, 1);

                    _show_skill_use_messages(obj, skill, target, successCount, skillBonus);
                    giveExp = false;
                }
            }

            int skillValue = skillGetValue(obj, skill);
            int roll = randomRoll(skillValue, skillOrCritSuccessBonus, &hpToHeal);

            if (roll == ROLL_SUCCESS || roll == ROLL_CRITICAL_SUCCESS) {
                hpToHeal = randomBetween(minimumHpToHeal + 4, maximumHpToHeal + 10);
                critterAdjustHitPoints(target, hpToHeal);

                if (obj == gDude) {
                    // You heal %d hit points.
                    messageListItem.num = 500;
                    if (!messageListGetItem(&gSkillsMessageList, &messageListItem)) {
                        return -1;
                    }

                    if (maximumHp - currentHp < hpToHeal) {
                        hpToHeal = maximumHp - currentHp;
                    }
                    snprintf(text, sizeof(text), messageListItem.text, hpToHeal);
                    displayMonitorAddMessage(text);
                } else if (mpSkillRemoteNetId(obj) != 0) {
                    MpSendSkillUseFeedback(mpSkillRemoteNetId(obj), 500, hpToHeal, 0, 1);
                }

                if (gMpActive && gMpIsHost && target != obj && target != gDude) {
                    uint8_t targetNetId = MpCombatGetCritterPlayerNetId(target);
                    if (targetNetId != 0 && targetNetId != mpSkillRemoteNetId(obj)) {
                        char targetNotify[256];
                        snprintf(targetNotify, sizeof(targetNotify), "%s repaired you (+%d HP).", objectGetName(obj), hpToHeal);
                        MpCombatSendMonitorMessageToPlayer(targetNetId, targetNotify);
                    }
                }

                if (!skillUseSlotAdded) {
                    skillUpdateLastUse(obj, SKILL_REPAIR);
                }

                target->data.critter.combat.maneuver &= ~CRITTER_MANUEVER_FLEEING;

                if (target == gDude) {
                    interfaceRenderHitPoints(true);
                }

                successCount = 1;
                _show_skill_use_messages(obj, skill, target, successCount, skillBonus);
                scriptsExecMapUpdateProc();
                if (mpSkillRemoteNetId(obj) == 0) {
                    paletteFadeTo(_cmap);
                }

                giveExp = false;
            } else {
                // You fail to do any healing.
                messageListItem.num = 503;
                if (!messageListGetItem(&gSkillsMessageList, &messageListItem)) {
                    return -1;
                }

                snprintf(text, sizeof(text), messageListItem.text, hpToHeal);
                mpSkillFeedback(obj, &messageListItem, hpToHeal, 0, 1);

                scriptsExecMapUpdateProc();
                if (mpSkillRemoteNetId(obj) == 0) {
                    paletteFadeTo(_cmap);
                }
            }
        } else {
            if (obj == gDude) {
                // 501: You look healty already
                // 502: %s looks healthy already
                messageListItem.num = (target == gDude ? 501 : 502);
                if (!messageListGetItem(&gSkillsMessageList, &messageListItem)) {
                    return -1;
                }

                snprintf(text, sizeof(text), messageListItem.text, objectGetName(target));
                displayMonitorAddMessage(text);

                giveExp = false;
            } else if (mpSkillRemoteNetId(obj) != 0) {
                uint8_t perfNetId = mpSkillRemoteNetId(obj);
                MpSendSkillUseFeedback(perfNetId,
                    (target == gDude ? 501 : 502), 0,
                    target != gDude ? MpGetObjNetId(target) : 0, 0);
                giveExp = false;
            }
        }

        if (obj == gDude) {
            gameTimeAddSeconds(1800 * damageHealingAttempts);
        }

        break;
    default:
        messageListItem.num = 510; // skill_use: invalid skill used.
        if (messageListGetItem(&gSkillsMessageList, &messageListItem)) {
            debugPrint(messageListItem.text);
        }

        return -1;
    }

    if (giveExp) {
        _show_skill_use_messages(obj, skill, target, successCount, skillBonus);
    }

    if (skill == SKILL_FIRST_AID || skill == SKILL_DOCTOR) {
        scriptsExecMapUpdateProc();
    }

    return 0;
}

// 0x4ABBE4
SkillStealResult skillsPerformStealing(Object* thief, Object* target, Object* item, int quantity, bool isPlanting, int* xpOverride)
{
    assert(thief != nullptr);
    assert(target != nullptr);
    assert(item != nullptr);
    assert(quantity >= 0);
    assert(xpOverride != nullptr);

    *xpOverride = -1;

    int hookXpOverride = -1;
    int hookResult = scriptHooks_Steal(thief, target, item, isPlanting, quantity, &hookXpOverride);
    if (hookXpOverride >= 0) {
        *xpOverride = hookXpOverride;
    }

    if (hookResult == static_cast<int>(SkillStealResult::Fail)) {
        return SkillStealResult::Fail;
    }

    if (hookResult == static_cast<int>(SkillStealResult::Success) || hookResult == static_cast<int>(SkillStealResult::Caught)) {
        return static_cast<SkillStealResult>(hookResult);
    }

    int howMuch;

    int stealModifier = -_gStealCount + 1;

    // Co-op: every player is a "dude" — the Pickpocket perk must exempt a
    // remote thief's roll the same way it exempts the host's.
    if ((thief != gDude && !MpProfileIsNetworkPlayer(thief)) || !perkHasRank(thief, PERK_PICKPOCKET)) {
        // -4% per item size
        stealModifier -= 4 * itemGetSize(item);

        if (objectTypeFromFid(target->fid) == OBJ_TYPE_CRITTER) {
            // check facing: -25% if face to face
            if (_is_hit_from_front(thief, target)) {
                stealModifier -= 25;
            }
        }
    }

    if ((target->data.critter.combat.results & (DAM_KNOCKED_OUT | DAM_KNOCKED_DOWN)) != 0) {
        stealModifier += 20;
    }

    int stealChance = stealModifier + skillGetValue(thief, SKILL_STEAL);
    if (stealChance > 95) {
        stealChance = 95;
    }

    int stealRoll;
    // Co-op: remote players stealing from the shared party get the same
    // auto-success as the host's dude (the vanilla party-trade flow).
    if ((thief == gDude || MpProfileIsNetworkPlayer(thief)) && objectIsPartyMember(target)) {
        stealRoll = ROLL_CRITICAL_SUCCESS;
    } else {
        int criticalChance = critterGetStat(thief, STAT_CRITICAL_CHANCE);
        stealRoll = randomRoll(stealChance, criticalChance, &howMuch);
    }

    int catchRoll;
    if (stealRoll == ROLL_CRITICAL_SUCCESS) {
        catchRoll = ROLL_CRITICAL_FAILURE;
    } else if (stealRoll == ROLL_CRITICAL_FAILURE) {
        catchRoll = ROLL_SUCCESS;
    } else {
        int catchChance;
        if (objectTypeFromPid(target->pid) == OBJ_TYPE_CRITTER) {
            catchChance = skillGetValue(target, SKILL_STEAL) - stealModifier;
        } else {
            catchChance = 30 - stealModifier;
        }

        catchRoll = randomRoll(catchChance, 0, &howMuch);
    }

    // CE: skip "You steal/plant the..." messages when using steal to trade with companions
    bool skipMessages = objectIsPartyMember(target);
    MessageListItem messageListItem;
    char text[60];

    if (catchRoll != ROLL_SUCCESS && catchRoll != ROLL_CRITICAL_SUCCESS) {
        // 571: You steal the %s.
        // 573: You plant the %s.
        messageListItem.num = isPlanting ? 573 : 571;
        if (!skipMessages && messageListGetItem(&gSkillsMessageList, &messageListItem)) {
            snprintf(text, sizeof(text), messageListItem.text, objectGetName(item));
            if (gSkillRemoteMessageSink != nullptr) {
                snprintf(gSkillRemoteMessageSink, gSkillRemoteMessageSinkSize, "%s", text);
            } else {
                displayMonitorAddMessage(text);
            }
        }

        return SkillStealResult::Success;
    } else {
        // 570: You're caught stealing the %s.
        // 572: You're caught planting the %s.
        messageListItem.num = isPlanting ? 572 : 570;
        if (!skipMessages && messageListGetItem(&gSkillsMessageList, &messageListItem)) {
            snprintf(text, sizeof(text), messageListItem.text, objectGetName(item));
            if (gSkillRemoteMessageSink != nullptr) {
                snprintf(gSkillRemoteMessageSink, gSkillRemoteMessageSinkSize, "%s", text);
            } else {
                displayMonitorAddMessage(text);
            }
        }

        return SkillStealResult::Caught;
    }
}

// 0x4ABDEC
int skillGetGameDifficultyModifier(Skill skill)
{
    switch (skill) {
    case SKILL_FIRST_AID:
    case SKILL_DOCTOR:
    case SKILL_SNEAK:
    case SKILL_LOCKPICK:
    case SKILL_STEAL:
    case SKILL_TRAPS:
    case SKILL_SCIENCE:
    case SKILL_REPAIR:
    case SKILL_SPEECH:
    case SKILL_BARTER:
    case SKILL_GAMBLING:
    case SKILL_OUTDOORSMAN: {
        int gameDifficulty = settings.preferences.game_difficulty;

        if (gameDifficulty == GAME_DIFFICULTY_HARD) {
            return -10;
        } else if (gameDifficulty == GAME_DIFFICULTY_EASY) {
            return 20;
        }
    }
    default:
        return 0;
    }
}

// Co-op: usage-slot times are per player. The host's own dude (and
// singleplayer) keep using the vanilla global table (saved in the savegame);
// remote players' times live in their synced profile so each player enforces
// their own 3-uses-per-24h limit.
static int32_t skillUsageTime(Object* critter, Skill skill, int slot)
{
    if (critter != nullptr && critter != gDude && MpProfileIsNetworkPlayer(critter)) {
        MpPlayerRuntime* runtime = MpProfileFindRuntimeByObject(critter);
        if (runtime != nullptr) {
            return runtime->profile.skillUseTimes[skill][slot];
        }
    }
    return _timesSkillUsed[skill][slot];
}

static void skillUsageTimeSet(Object* critter, Skill skill, int slot, int32_t value)
{
    if (critter != nullptr && critter != gDude && MpProfileIsNetworkPlayer(critter)) {
        MpPlayerRuntime* runtime = MpProfileFindRuntimeByObject(critter);
        if (runtime != nullptr) {
            runtime->profile.skillUseTimes[skill][slot] = value;
            return;
        }
    }
    _timesSkillUsed[skill][slot] = value;
}

// 0x4ABE44
static int skillGetFreeUsageSlot(Object* critter, Skill skill)
{
    for (int slot = 0; slot < SKILLS_MAX_USES_PER_DAY; slot++) {
        if (skillUsageTime(critter, skill, slot) == 0) {
            return slot;
        }
    }

    unsigned int time = gameTimeGetTime();
    int hoursSinceLastUsage = (time - skillUsageTime(critter, skill, 0)) / GAME_TIME_TICKS_PER_HOUR;
    if (hoursSinceLastUsage <= 24) {
        return -1;
    }

    return SKILLS_MAX_USES_PER_DAY - 1;
}

// 0x4ABEB8
int skillUpdateLastUse(Object* critter, Skill skill)
{
    int slot = skillGetFreeUsageSlot(critter, skill);
    if (slot == -1) {
        return -1;
    }

    if (skillUsageTime(critter, skill, slot) != 0) {
        for (int i = 0; i < slot; i++) {
            skillUsageTimeSet(critter, skill, i, skillUsageTime(critter, skill, i + 1));
        }
    }

    skillUsageTimeSet(critter, skill, slot, gameTimeGetTime());

    return 0;
}

// NOTE: Inlined.
//
// 0x4ABF24
int skill_use_slot_clear()
{
    memset(_timesSkillUsed, 0, sizeof(_timesSkillUsed));
    return 0;
}

// 0x4ABF3C
int skillsUsageSave(File* stream)
{
    return fileWriteInt32List(stream, (int*)_timesSkillUsed, SKILL_COUNT * SKILLS_MAX_USES_PER_DAY);
}

// 0x4ABF5C
int skillsUsageLoad(File* stream)
{
    return fileReadInt32List(stream, (int*)_timesSkillUsed, SKILL_COUNT * SKILLS_MAX_USES_PER_DAY);
}

// 0x4ABF7C
char* skillsGetGenericResponse(Object* critter, bool isDude)
{
    int baseMessageId;
    int count;

    if (isDude) {
        baseMessageId = 1100;
        count = 4;
    } else {
        baseMessageId = 1000;
        count = 5;
    }

    int messageId = randomBetween(0, count);

    MessageListItem messageListItem;
    char* msg = getmsg(&gSkillsMessageList, &messageListItem, baseMessageId + messageId);
    return msg;
}

} // namespace fallout
