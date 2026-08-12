#include "multiplayer_log.h"

#include <stdio.h>

#include "config.h"
#include "debug.h"
#include "game_config.h"

namespace fallout {

static uint32_t gMpLogMask = MP_LOG_ALL;

const char* MpLogCategoryTag(MpLogCategory cat)
{
    switch (cat) {
    case MP_LOG_LIFECYCLE:
        return "LIFE";
    case MP_LOG_HANDSHAKE:
        return "HSK";
    case MP_LOG_NET:
        return "NET";
    case MP_LOG_SYNC:
        return "SYNC";
    case MP_LOG_PROFILE:
        return "PROF";
    case MP_LOG_MODEL:
        return "MODEL";
    case MP_LOG_COMBAT:
        return "COMBAT";
    case MP_LOG_CHAT:
        return "CHAT";
    case MP_LOG_UI:
        return "UI";
    case MP_LOG_INPUT:
        return "INPUT";
    case MP_LOG_OBJECT:
        return "OBJ";
    case MP_LOG_STATS:
        return "STATS";
    case MP_LOG_SCRIPT:
        return "SCRIPT";
    case MP_LOG_VOTE:
        return "VOTE";
    case MP_LOG_DIALOG:
        return "DIALOG";
    case MP_LOG_LOOT:
        return "LOOT";
    case MP_LOG_MOVIE:
        return "MOVIE";
    case MP_LOG_WORLDMAP:
        return "WORLDMAP";
    case MP_LOG_MISC:
        return "MISC";
    }
    return "?";
}

bool MpLogEnabled(MpLogCategory cat)
{
    return (gMpLogMask & static_cast<uint32_t>(cat)) != 0;
}

void MpLogSetEnabled(MpLogCategory cat, bool enabled)
{
    if (enabled) {
        gMpLogMask |= static_cast<uint32_t>(cat);
    } else {
        gMpLogMask &= ~static_cast<uint32_t>(cat);
    }
}

uint32_t MpLogMask()
{
    return gMpLogMask;
}

void MpLogSetMask(uint32_t mask)
{
    gMpLogMask = mask;
}

static void mpLogV(MpLogCategory cat, const char* format, va_list args)
{
    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), format, args);
    debugFilePrint("MP[%s]: %s", MpLogCategoryTag(cat), buffer);
}

void MpLog(MpLogCategory cat, const char* format, ...)
{
    if (!MpLogEnabled(cat)) {
        return;
    }
    va_list args;
    va_start(args, format);
    mpLogV(cat, format, args);
    va_end(args);
}

void MpLogAlways(MpLogCategory cat, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    mpLogV(cat, format, args);
    va_end(args);
}

void MpLogInit()
{
    // Default: everything on. The [debug] section narrows it down.
    gMpLogMask = MP_LOG_ALL;

    bool all = true;
    configGetBool(&gGameConfig, "debug", "coop_log_all", &all, true);
    if (!all) {
        gMpLogMask = 0;
    }

    struct {
        MpLogCategory cat;
        const char* key;
    } keys[] = {
        { MP_LOG_LIFECYCLE, "coop_log_lifecycle" },
        { MP_LOG_HANDSHAKE, "coop_log_handshake" },
        { MP_LOG_NET, "coop_log_net" },
        { MP_LOG_SYNC, "coop_log_sync" },
        { MP_LOG_PROFILE, "coop_log_profile" },
        { MP_LOG_MODEL, "coop_log_model" },
        { MP_LOG_COMBAT, "coop_log_combat" },
        { MP_LOG_CHAT, "coop_log_chat" },
        { MP_LOG_UI, "coop_log_ui" },
        { MP_LOG_INPUT, "coop_log_input" },
        { MP_LOG_OBJECT, "coop_log_object" },
        { MP_LOG_STATS, "coop_log_stats" },
        { MP_LOG_SCRIPT, "coop_log_script" },
        { MP_LOG_VOTE, "coop_log_vote" },
        { MP_LOG_DIALOG, "coop_log_dialog" },
        { MP_LOG_LOOT, "coop_log_loot" },
        { MP_LOG_MOVIE, "coop_log_movie" },
        { MP_LOG_WORLDMAP, "coop_log_worldmap" },
        { MP_LOG_MISC, "coop_log_misc" },
    };

    for (const auto& entry : keys) {
        bool enabled = all;
        configGetBool(&gGameConfig, "debug", entry.key, &enabled, all);
        MpLogSetEnabled(entry.cat, enabled);
    }

    debugFilePrint("MP[LIFE]: log mask=0x%08X", gMpLogMask);
}

} // namespace fallout
