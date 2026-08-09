#include "multiplayer_profile.h"

#include <algorithm>
#include <array>
#include <functional>
#include <stdio.h>
#include <string.h>
#include <unordered_map>
#include <unordered_set>

#include "art.h"
#include "animation.h"
#include "character_editor.h"
#include "critter.h"
#include "db.h"
#include "debug.h"
#include "inventory.h"
#include "item.h"
#include "memory.h"
#include "multiplayer.h"
#include "object.h"
#include "perk.h"
#include "proto.h"
#include "skill.h"
#include "stat.h"
#include "trait.h"
#include "platform_compat.h"

namespace fallout {

// Forward: destroys an object's inventory tree (children first). Defined
// below; used by the runtime-update reapply.
static void mpProfileDestroyObjectItems(Object* obj);

namespace {

constexpr uint32_t kProfileMagic = 0x4D505246; // MPRF
constexpr size_t kProfileHeaderSize = 20;

class Writer {
public:
    void u8(uint8_t value) { _data.push_back(value); }

    void u16(uint16_t value)
    {
        _data.push_back((uint8_t)(value & 0xFF));
        _data.push_back((uint8_t)((value >> 8) & 0xFF));
    }

    void u32(uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8) {
            _data.push_back((uint8_t)((value >> shift) & 0xFF));
        }
    }

    void i32(int32_t value) { u32((uint32_t)value); }

    void bytes(const void* data, size_t size)
    {
        if (data != nullptr && size != 0) {
            const uint8_t* begin = (const uint8_t*)data;
            _data.insert(_data.end(), begin, begin + size);
        }
    }

    const std::vector<uint8_t>& data() const { return _data; }

private:
    std::vector<uint8_t> _data;
};

class Reader {
public:
    Reader(const void* data, size_t size)
        : _data((const uint8_t*)data)
        , _size(size)
    {
    }

    bool u8(uint8_t* value)
    {
        if (value == nullptr || _remaining() < 1) return false;
        *value = _data[_offset++];
        return true;
    }

    bool u16(uint16_t* value)
    {
        if (value == nullptr || _remaining() < 2) return false;
        *value = (uint16_t)_data[_offset]
            | ((uint16_t)_data[_offset + 1] << 8);
        _offset += 2;
        return true;
    }

    bool u32(uint32_t* value)
    {
        if (value == nullptr || _remaining() < 4) return false;
        *value = (uint32_t)_data[_offset]
            | ((uint32_t)_data[_offset + 1] << 8)
            | ((uint32_t)_data[_offset + 2] << 16)
            | ((uint32_t)_data[_offset + 3] << 24);
        _offset += 4;
        return true;
    }

    bool i32(int32_t* value)
    {
        uint32_t raw;
        if (!u32(&raw) || value == nullptr) return false;
        *value = (int32_t)raw;
        return true;
    }

    bool bytes(void* output, size_t size)
    {
        if (output == nullptr || _remaining() < size) return false;
        memcpy(output, _data + _offset, size);
        _offset += size;
        return true;
    }

    bool skip(size_t size)
    {
        if (_remaining() < size) return false;
        _offset += size;
        return true;
    }

    size_t remaining() const { return _remaining(); }

private:
    size_t _remaining() const { return _offset <= _size ? _size - _offset : 0; }

    const uint8_t* _data = nullptr;
    size_t _size = 0;
    size_t _offset = 0;
};

static uint32_t hashBytes(const void* data, size_t size, uint32_t hash = 0x811C9DC5u)
{
    const uint8_t* bytes = (const uint8_t*)data;
    for (size_t index = 0; index < size; index++) {
        hash ^= bytes[index];
        hash *= 0x01000193u;
    }
    return hash;
}

static std::unordered_map<uint8_t, MpPlayerRuntime> gRuntimes;
static std::unordered_map<const Object*, uint8_t> gObjectToRuntime;

static bool captureInventoryNode(const Object* object, MpPlayerProfile* profile,
    std::unordered_map<const Object*, uint32_t>* seen, uint32_t* id, size_t depth)
{
    if (object == nullptr || profile == nullptr || seen == nullptr || id == nullptr
        || depth > MP_PROFILE_MAX_INVENTORY_DEPTH) {
        debugFilePrint("MPROF: capture node failed arg/depth=%zu", depth);
        return false;
    }
    auto found = seen->find(object);
    if (found != seen->end()) {
        *id = found->second;
        return true;
    }
    if (profile->inventory.size() >= MP_PROFILE_MAX_INVENTORY_NODES) {
        debugFilePrint("MPROF: capture node failed node limit=%zu", profile->inventory.size());
        return false;
    }

    MpInventoryNode node;
    node.id = (uint32_t)profile->inventory.size() + 1;
    node.pid = object->pid;
    node.fid = object->fid;
    node.frame = object->frame;
    node.rotation = object->rotation;
    node.flags = object->flags & (OBJECT_IN_ANY_HAND | OBJECT_WORN | OBJECT_EQUIPPED
        | OBJECT_OPEN_DOOR | OBJECT_NO_BLOCK | OBJECT_LIGHT_THRU);
    node.dataFlags = object->data.flags;
    node.lightDistance = object->lightDistance;
    node.lightIntensity = object->lightIntensity;
    (*seen)[object] = node.id;
    size_t nodeIndex = profile->inventory.size();
    profile->inventory.push_back(node);

    if (objectTypeFromPid(object->pid) == OBJ_TYPE_ITEM) {
        int type = itemGetType(const_cast<Object*>(object));
        switch (type) {
        case ITEM_TYPE_WEAPON:
            profile->inventory.back().weaponAmmoQuantity = object->data.item.weapon.ammoQuantity;
            profile->inventory.back().weaponAmmoTypePid = object->data.item.weapon.ammoTypePid;
            break;
        case ITEM_TYPE_AMMO:
            profile->inventory.back().ammoQuantity = object->data.item.ammo.quantity;
            break;
        case ITEM_TYPE_MISC:
            profile->inventory.back().miscCharges = object->data.item.misc.charges;
            break;
        case ITEM_TYPE_KEY:
            profile->inventory.back().keyCode = object->data.item.key.keyCode;
            break;
        default:
            break;
        }
    }

    for (int index = 0; index < object->data.inventory.length; index++) {
        const InventoryItem* inventoryItem = &object->data.inventory.items[index];
        if (inventoryItem->item == nullptr || inventoryItem->quantity < 1) {
            debugFilePrint("MPROF: capture node invalid child item idx=%d item=%p qty=%d",
                index, (void*)inventoryItem->item, inventoryItem->quantity);
            return false;
        }
        uint32_t childId = 0;
        if (!captureInventoryNode(inventoryItem->item, profile, seen, &childId, depth + 1)) {
            return false;
        }
        // A shared inventory object is invalid for a portable profile. It
        // would make the reconstructed ownership graph ambiguous.
        if (std::find(profile->inventory[nodeIndex].children.begin(),
                profile->inventory[nodeIndex].children.end(), childId)
            != profile->inventory[nodeIndex].children.end()) {
            debugFilePrint("MPROF: capture node duplicate child id=%u", childId);
            return false;
        }
        profile->inventory[nodeIndex].children.push_back(childId);
        profile->inventory[childId - 1].quantity = inventoryItem->quantity;
    }

    *id = node.id;
    return true;
}

static bool captureCommon(const Object* object, MpPlayerProfile* profile)
{
    if (object == nullptr || profile == nullptr || objectTypeFromPid(object->pid) != OBJ_TYPE_CRITTER) {
        debugFilePrint("MPROF: capture common failed obj=%p critter=%d",
            (void*)object, object != nullptr ? (objectTypeFromPid(object->pid) == OBJ_TYPE_CRITTER) : -1);
        return false;
    }

    Proto* proto = nullptr;
    if (protoGetProto(object->pid, &proto) == -1 || proto == nullptr) {
        debugFilePrint("MPROF: capture common protoGetProto failed pid=0x%X", object->pid);
        return false;
    }

    profile->prototypeMessageId = proto->critter.messageId;
    profile->prototypeFlags = proto->critter.flags;
    profile->prototypeExtendedFlags = proto->critter.extendedFlags;
    profile->prototypeLightDistance = proto->critter.lightDistance;
    profile->prototypeLightIntensity = proto->critter.lightIntensity;
    profile->prototypeHeadFid = proto->critter.headFid;
    profile->prototypeFid = proto->critter.fid;
    profile->prototypeAiPacket = proto->critter.aiPacket;
    profile->prototypeTeam = proto->critter.team;
    profile->critterFlags = proto->critter.data.flags;
    memcpy(profile->baseStats, proto->critter.data.baseStats, sizeof(profile->baseStats));
    memcpy(profile->bonusStats, proto->critter.data.bonusStats, sizeof(profile->bonusStats));
    memcpy(profile->skills, proto->critter.data.skills, sizeof(profile->skills));
    profile->bodyType = proto->critter.data.bodyType;
    profile->experience = proto->critter.data.experience;
    profile->killType = proto->critter.data.killType;
    profile->damageType = proto->critter.data.damageType;

    profile->tile = object->tile;
    profile->x = object->x;
    profile->y = object->y;
    profile->sx = object->sx;
    profile->sy = object->sy;
    profile->frame = object->frame;
    profile->rotation = object->rotation;
    profile->fid = object->fid;
    profile->flags = object->flags;
    profile->elevation = object->elevation;
    profile->lightDistance = object->lightDistance;
    profile->lightIntensity = object->lightIntensity;
    profile->hp = object->data.critter.hp;
    profile->radiation = object->data.critter.radiation;
    profile->poison = object->data.critter.poison;
    profile->reaction = object->data.critter.reaction;
    profile->combatManeuver = object->data.critter.combat.maneuver;
    profile->combatAp = object->data.critter.combat.ap;
    profile->combatResults = object->data.critter.combat.results;
    profile->combatDamageLastTurn = object->data.critter.combat.damageLastTurn;
    profile->combatAiPacket = object->data.critter.combat.aiPacket;
    profile->combatTeam = object->data.critter.combat.team;

    char* name = critterGetName(const_cast<Object*>(object));
    if (name != nullptr) {
        memset(profile->name, 0, sizeof(profile->name));
        strncpy(profile->name, name, MP_PROFILE_NAME_LENGTH - 1);
    }
    profile->name[MP_PROFILE_NAME_LENGTH - 1] = '\0';

    int modelId = proto->critter.fid & 0xFFF;
    if (artCopyFileName(OBJ_TYPE_CRITTER, modelId, profile->modelName) != 0) {
        profile->modelName[0] = '\0';
    }
    profile->modelName[12] = '\0';
    return true;
}

static uint32_t hashModelFiles(const std::vector<MpModelFile>& files)
{
    uint32_t hash = 0x811C9DC5u;
    for (const MpModelFile& file : files) {
        hash = hashBytes(file.path.data(), file.path.size(), hash);
        hash = hashBytes(file.data.data(), file.data.size(), hash);
    }
    return hash;
}

static bool captureModelFiles(MpPlayerProfile* profile)
{
    if (profile == nullptr || profile->modelName[0] == '\0') {
        debugFilePrint("MPROF: capture model files skipped (no model name)");
        return true;
    }

    int modelId = profile->prototypeFid & 0xFFF;
    std::unordered_set<std::string> seenPaths;
    size_t totalBytes = 0;
    int probed = 0;
    int found = 0;
    for (int animation = 0; animation < ANIM_COUNT; animation++) {
        for (int weapon = 0; weapon < WEAPON_ANIMATION_COUNT; weapon++) {
            for (int rotation = 0; rotation < ROTATION_COUNT; rotation++) {
                int fid = buildFid(OBJ_TYPE_CRITTER, modelId, animation, weapon, static_cast<Rotation>(rotation));
                char* fullPath = artBuildFilePath(fid);
                if (fullPath == nullptr) continue;
                probed++;
                const char* slash = strrchr(fullPath, '\\');
                if (slash == nullptr) slash = strrchr(fullPath, '/');
                if (slash == nullptr || slash[1] == '\0') continue;
                std::string fileName = slash + 1;
                if (seenPaths.find(fileName) != seenPaths.end()) continue;
                int size = 0;
                if (dbGetFileSize(fullPath, &size) != 0 || size <= 0) continue;
                if (size_t(size) + totalBytes > MP_PROFILE_MAX_BYTES / 2) {
                    debugFilePrint("MPROF: capture model files failed size cap file=%s size=%d total=%zu",
                        fileName.c_str(), size, totalBytes);
                    return false;
                }
                MpModelFile file;
                file.path = fileName;
                file.data.resize(size);
                if (dbGetFileContents(fullPath, file.data.data()) != 0) {
                    debugFilePrint("MPROF: capture model files dbGetFileContents failed file=%s size=%d",
                        fileName.c_str(), size);
                    return false;
                }
                seenPaths.insert(file.path);
                totalBytes += file.data.size();
                found++;
                profile->modelFiles.push_back(std::move(file));
            }
        }
    }
    profile->modelHash = hashModelFiles(profile->modelFiles);
    debugFilePrint("MPROF: capture model files done modelId=%d name=%s probed=%d files=%d bytes=%zu hash=%08X",
        modelId, profile->modelName, probed, found, totalBytes, profile->modelHash);
    return true;
}

static bool isSafeModelFilePath(const std::string& path)
{
    if (path.empty() || path.size() > 64 || path.find("..") != std::string::npos
        || path.find('/') != std::string::npos || path.find('\\') != std::string::npos) {
        return false;
    }
    // Critter art ships as .frm files plus per-animation variants named
    // .fr0-.fr7 (standing, walking, attacking, ...). All of them are valid
    // model payload files.
    size_t length = path.size();
    if (length >= 4 && compat_stricmp(path.c_str() + length - 4, ".frm") == 0) {
        return true;
    }
    if (length >= 4 && compat_stricmp(path.c_str() + length - 4, ".fr0") == 0) return true;
    if (length >= 4 && compat_stricmp(path.c_str() + length - 4, ".fr1") == 0) return true;
    if (length >= 4 && compat_stricmp(path.c_str() + length - 4, ".fr2") == 0) return true;
    if (length >= 4 && compat_stricmp(path.c_str() + length - 4, ".fr3") == 0) return true;
    if (length >= 4 && compat_stricmp(path.c_str() + length - 4, ".fr4") == 0) return true;
    if (length >= 4 && compat_stricmp(path.c_str() + length - 4, ".fr5") == 0) return true;
    if (length >= 4 && compat_stricmp(path.c_str() + length - 4, ".fr6") == 0) return true;
    if (length >= 4 && compat_stricmp(path.c_str() + length - 4, ".fr7") == 0) return true;
    return false;
}

// Creates every path component of [path], tolerating components that already
// exist. compat_mkdir_recursive is NOT idempotent: it aborts the whole walk
// when mkdir returns any error, including EEXIST, so a directory tree left
// behind by an earlier session makes it fail forever.
static bool mpEnsureDirectoryTree(const char* path)
{
    if (path == nullptr || path[0] == '\0') {
        return false;
    }

    char buffer[COMPAT_MAX_PATH];
    strncpy(buffer, path, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    char drive[COMPAT_MAX_DRIVE];
    compat_splitpath(buffer, drive, nullptr, nullptr, nullptr);
    char* cursor = buffer + strlen(drive);
    if (*cursor == '\\' || *cursor == '/') {
        cursor++;
    }
    for (; *cursor != '\0'; cursor++) {
        if (*cursor == '\\' || *cursor == '/') {
            char saved = *cursor;
            *cursor = '\0';
            if (compat_mkdir(buffer) != 0 && !compat_is_dir(buffer)) {
                return false;
            }
            *cursor = saved;
        }
    }
    if (compat_mkdir(buffer) != 0 && !compat_is_dir(buffer)) {
        return false;
    }
    return true;
}

static bool installModelFiles(MpPlayerProfile* profile)
{
    if (profile == nullptr || profile->modelName[0] == '\0') return true;
    if (profile->modelFiles.empty()) {
        int modelId = artListIndex(OBJ_TYPE_CRITTER, profile->modelName);
        if (modelId >= 0) {
            profile->localModelIndex = modelId;
            debugFilePrint("MPROF: install model by name='%s' index=%d", profile->modelName, modelId);
            return true;
        }
        // Custom model whose payload was skipped on the wire (the receiver
        // was assumed to have it): pull it from the session registry.
        if (!MpModelRegistryResolve(profile->modelHash, &profile->modelFiles)) {
            debugFilePrint("MPROF: install model failed name='%s' hash=%08X not in registry",
                profile->modelName, profile->modelHash);
            return false;
        }
        debugFilePrint("MPROF: install model from registry name='%s' hash=%08X files=%zu",
            profile->modelName, profile->modelHash, profile->modelFiles.size());
    }
    if (hashModelFiles(profile->modelFiles) != profile->modelHash) {
        debugFilePrint("MPROF: install model failed hash mismatch name='%s'", profile->modelName);
        return false;
    }
    // Remember the payload so future skipped-model profiles resolve locally.
    MpModelRegistryRemember(profile->modelHash, profile->modelFiles);

    // File operations need the root WITHOUT a trailing backslash
    // (compat_mkdir_recursive fails on a trailing separator), but the art
    // registry root must END with one: artBuildFilePath builds the lookup
    // path as "<root><art>\<critters>\<name><codes>" and does not insert a
    // separator itself.
    char root[COMPAT_MAX_PATH];
    snprintf(root, sizeof(root), "%smp_models\\%08X", _cd_path_base, profile->modelHash);
    char rootWithSeparator[COMPAT_MAX_PATH];
    snprintf(rootWithSeparator, sizeof(rootWithSeparator), "%s\\", root);
    char critterDir[COMPAT_MAX_PATH];
    snprintf(critterDir, sizeof(critterDir), "%s\\art\\critters", root);
    if (!mpEnsureDirectoryTree(critterDir)) {
        debugFilePrint("MPROF: install model mkdir failed dir='%s'", critterDir);
        return false;
    }
    for (const MpModelFile& file : profile->modelFiles) {
        if (!isSafeModelFilePath(file.path) || file.data.empty()
            || file.data.size() > MP_PROFILE_MAX_BYTES / 2) {
            debugFilePrint("MPROF: install model bad file '%s' bytes=%zu", file.path.c_str(), file.data.size());
            return false;
        }
        // The captured basename is "<artBase><weapon><anim>.fr<N>" where
        // artBase is the file prefix the SOURCE machine's art list resolves
        // to — which may differ from the logical model name when the source
        // uses an art alias (e.g. 'hmwarr' aliased to 'hmjmps' frames). The
        // session registry maps OUR modelName to the FID model index, and
        // artBuildFilePath appends the two code chars + ".fr<N>" to that
        // name, so the frames must be stored under OUR name or artLock will
        // fail (invisible avatar, movement registration rejected).
        if (file.path.size() < 7) {
            debugFilePrint("MPROF: install model short name '%s'", file.path.c_str());
            return false;
        }
        std::string suffix = file.path.substr(file.path.size() - 6); // "<w><a>.fr<N>" or ".frm" tail
        char localName[COMPAT_MAX_PATH];
        snprintf(localName, sizeof(localName), "%s%s", profile->modelName, suffix.c_str());
        if (!isSafeModelFilePath(localName)) {
            debugFilePrint("MPROF: install model unsafe local name '%s' (from '%s')",
                localName, file.path.c_str());
            return false;
        }
        char path[COMPAT_MAX_PATH];
        snprintf(path, sizeof(path), "%s\\%s", critterDir, localName);
        FILE* stream = compat_fopen(path, "wb");
        if (stream == nullptr) {
            debugFilePrint("MPROF: install model fopen failed path='%s'", path);
            return false;
        }
        size_t written = fwrite(file.data.data(), 1, file.data.size(), stream);
        fclose(stream);
        if (written != file.data.size()) {
            debugFilePrint("MPROF: install model write failed path='%s' wrote=%zu want=%zu",
                path, written, file.data.size());
            return false;
        }
    }
    int modelId = artRegisterSessionCritterModel(profile->modelName, rootWithSeparator, 0, 1);
    if (modelId < 0) {
        debugFilePrint("MPROF: install model register failed name='%s'", profile->modelName);
        return false;
    }
    profile->localModelIndex = modelId;
    profile->prototypeFid = (profile->prototypeFid & ~0xFFF) | modelId;
    profile->fid = (profile->fid & ~0xFFF) | modelId;
    profile->modelHash = hashModelFiles(profile->modelFiles);
    artCacheFlush();
    debugFilePrint("MPROF: install model done name='%s' files=%zu index=%d root='%s'",
        profile->modelName, profile->modelFiles.size(), modelId, root);
    return true;
}

static bool applyItemNode(const MpPlayerProfile& profile, uint32_t id,
    Object* owner, std::unordered_map<uint32_t, Object*>* built, size_t depth)
{
    if (owner == nullptr || built == nullptr || id == 0 || id > profile.inventory.size()
        || depth > MP_PROFILE_MAX_INVENTORY_DEPTH) {
        return false;
    }
    const MpInventoryNode& node = profile.inventory[id - 1];
    if (node.id != id || node.pid < 0 || objectTypeFromPid(node.pid) != OBJ_TYPE_ITEM
        || node.quantity < 1) {
        return false;
    }
    if (built->find(id) != built->end()) {
        return false;
    }

    Proto* proto = nullptr;
    if (protoGetProto(node.pid, &proto) == -1 || proto == nullptr) {
        return false;
    }
    int fid = node.fid != 0 ? node.fid : proto->item.fid;
    Object* item = nullptr;
    if (objectCreateWithFidPid(&item, fid, node.pid) != 0 || item == nullptr) {
        return false;
    }
    item->fid = fid;
    item->frame = node.frame;
    item->rotation = static_cast<Rotation>(node.rotation);
    item->flags = node.flags;
    item->data.flags = node.dataFlags;
    item->lightDistance = node.lightDistance;
    item->lightIntensity = node.lightIntensity;
    item->sid = -1;
    item->scriptIndex = -1;
    switch (itemGetType(item)) {
    case ITEM_TYPE_WEAPON:
        item->data.item.weapon.ammoQuantity = node.weaponAmmoQuantity;
        item->data.item.weapon.ammoTypePid = node.weaponAmmoTypePid;
        break;
    case ITEM_TYPE_AMMO:
        item->data.item.ammo.quantity = node.ammoQuantity;
        break;
    case ITEM_TYPE_MISC:
        item->data.item.misc.charges = node.miscCharges;
        break;
    case ITEM_TYPE_KEY:
        item->data.item.key.keyCode = node.keyCode;
        break;
    default:
        break;
    }
    (*built)[id] = item;

    for (uint32_t childId : node.children) {
        if (!applyItemNode(profile, childId, item, built, depth + 1)) {
            return false;
        }
        Object* child = (*built)[childId];
        const MpInventoryNode& childNode = profile.inventory[childId - 1];
        if (itemAdd(item, child, childNode.quantity) != 0) {
            return false;
        }
    }

    Object* parentItem = item;
    if (itemAdd(owner, parentItem, node.quantity) != 0) {
        return false;
    }
    return true;
}

static bool readIntArray(Reader* reader, int32_t* values, size_t count)
{
    for (size_t index = 0; index < count; index++) {
        if (!reader->i32(&values[index])) return false;
    }
    return true;
}

static void writeIntArray(Writer* writer, const int32_t* values, size_t count)
{
    for (size_t index = 0; index < count; index++) writer->i32(values[index]);
}

static bool readProfileSectionIdentity(Reader* r, MpPlayerProfile* p)
{
    if (!r->bytes(p->name, sizeof(p->name))
        || !r->i32(&p->prototypeMessageId)
        || !r->i32(&p->prototypeFlags)
        || !r->i32(&p->prototypeExtendedFlags)
        || !r->i32(&p->prototypeLightDistance)
        || !r->i32(&p->prototypeLightIntensity)
        || !r->i32(&p->prototypeHeadFid)
        || !r->i32(&p->prototypeFid)
        || !r->i32(&p->prototypeAiPacket)
        || !r->i32(&p->prototypeTeam)
        || !r->i32(&p->critterFlags)
        || !r->i32(&p->bodyType)
        || !r->i32(&p->experience)
        || !r->i32(&p->killType)
        || !r->i32(&p->damageType)
        || !r->bytes(p->modelName, sizeof(p->modelName))) {
        return false;
    }
    p->name[MP_PROFILE_NAME_LENGTH - 1] = '\0';
    p->modelName[12] = '\0';
    return true;
}

static void writeProfileSectionIdentity(Writer* w, const MpPlayerProfile& p)
{
    w->bytes(p.name, sizeof(p.name));
    w->i32(p.prototypeMessageId);
    w->i32(p.prototypeFlags);
    w->i32(p.prototypeExtendedFlags);
    w->i32(p.prototypeLightDistance);
    w->i32(p.prototypeLightIntensity);
    w->i32(p.prototypeHeadFid);
    w->i32(p.prototypeFid);
    w->i32(p.prototypeAiPacket);
    w->i32(p.prototypeTeam);
    w->i32(p.critterFlags);
    w->i32(p.bodyType);
    w->i32(p.experience);
    w->i32(p.killType);
    w->i32(p.damageType);
    w->bytes(p.modelName, sizeof(p.modelName));
}

static bool readProfileSectionBaseStats(Reader* r, MpPlayerProfile* p)
{
    return readIntArray(r, p->baseStats, SAVEABLE_STAT_COUNT);
}

static void writeProfileSectionBaseStats(Writer* w, const MpPlayerProfile& p)
{
    writeIntArray(w, p.baseStats, SAVEABLE_STAT_COUNT);
}

static bool readProfileSectionBonusStats(Reader* r, MpPlayerProfile* p)
{
    return readIntArray(r, p->bonusStats, SAVEABLE_STAT_COUNT);
}

static void writeProfileSectionBonusStats(Writer* w, const MpPlayerProfile& p)
{
    writeIntArray(w, p.bonusStats, SAVEABLE_STAT_COUNT);
}

static bool readProfileSectionSkills(Reader* r, MpPlayerProfile* p)
{
    return readIntArray(r, p->skills, SKILL_COUNT);
}

static void writeProfileSectionSkills(Writer* w, const MpPlayerProfile& p)
{
    writeIntArray(w, p.skills, SKILL_COUNT);
}

static bool readProfileSectionPcStats(Reader* r, MpPlayerProfile* p)
{
    return readIntArray(r, p->pcStats, PC_STAT_COUNT)
        && r->i32(&p->editorLastLevel)
        && r->i32(&p->editorHasFreePerk)
        && r->i32(&p->remainingCharacterPoints);
}

static void writeProfileSectionPcStats(Writer* w, const MpPlayerProfile& p)
{
    writeIntArray(w, p.pcStats, PC_STAT_COUNT);
    w->i32(p.editorLastLevel);
    w->i32(p.editorHasFreePerk);
    w->i32(p.remainingCharacterPoints);
}

static bool readProfileSectionTagsTraits(Reader* r, MpPlayerProfile* p)
{
    return readIntArray(r, p->taggedSkills, NUM_TAGGED_SKILLS)
        && readIntArray(r, p->selectedTraits, TRAITS_MAX_SELECTED_COUNT);
}

static void writeProfileSectionTagsTraits(Writer* w, const MpPlayerProfile& p)
{
    writeIntArray(w, p.taggedSkills, NUM_TAGGED_SKILLS);
    writeIntArray(w, p.selectedTraits, TRAITS_MAX_SELECTED_COUNT);
}

static bool readProfileSectionPerks(Reader* r, MpPlayerProfile* p)
{
    return readIntArray(r, p->perkRanks, PERK_COUNT);
}

static void writeProfileSectionPerks(Writer* w, const MpPlayerProfile& p)
{
    writeIntArray(w, p.perkRanks, PERK_COUNT);
}

static bool readProfileSectionKills(Reader* r, MpPlayerProfile* p)
{
    return readIntArray(r, p->killCounts, KILL_TYPE_DEFAULT_COUNT);
}

static void writeProfileSectionKills(Writer* w, const MpPlayerProfile& p)
{
    writeIntArray(w, p.killCounts, KILL_TYPE_DEFAULT_COUNT);
}

static bool readProfileSectionSkillUse(Reader* r, MpPlayerProfile* p)
{
    return readIntArray(r, &p->skillUseTimes[0][0], SKILL_COUNT * 3)
        && r->i32(&p->sneakWorking);
}

static void writeProfileSectionSkillUse(Writer* w, const MpPlayerProfile& p)
{
    writeIntArray(w, &p.skillUseTimes[0][0], SKILL_COUNT * 3);
    w->i32(p.sneakWorking);
}

static bool readProfileSectionInventory(Reader* r, MpPlayerProfile* p)
{
    uint32_t rootCount;
    uint32_t nodeCount;
    if (!r->u32(&rootCount) || !r->u32(&nodeCount)
        || rootCount > MP_PROFILE_MAX_INVENTORY_NODES
        || nodeCount > MP_PROFILE_MAX_INVENTORY_NODES) {
        return false;
    }
    p->rootInventory.resize(rootCount);
    for (uint32_t& id : p->rootInventory) {
        if (!r->u32(&id)) return false;
    }
    p->inventory.clear();
    p->inventory.resize(nodeCount);
    for (MpInventoryNode& node : p->inventory) {
        uint32_t childCount;
        if (!r->u32(&node.id)
            || !r->i32(&node.pid)
            || !r->i32(&node.fid)
            || !r->i32(&node.frame)
            || !r->i32(&node.rotation)
            || !r->i32(&node.flags)
            || !r->i32(&node.dataFlags)
            || !r->i32(&node.quantity)
            || !r->i32(&node.lightDistance)
            || !r->i32(&node.lightIntensity)
            || !r->i32(&node.weaponAmmoQuantity)
            || !r->i32(&node.weaponAmmoTypePid)
            || !r->i32(&node.ammoQuantity)
            || !r->i32(&node.miscCharges)
            || !r->i32(&node.keyCode)
            || !r->u32(&childCount)
            || childCount > MP_PROFILE_MAX_INVENTORY_NODES) {
            return false;
        }
        node.children.resize(childCount);
        for (uint32_t& child : node.children) {
            if (!r->u32(&child)) return false;
        }
    }
    return true;
}

static void writeProfileSectionInventory(Writer* w, const MpPlayerProfile& p)
{
    w->u32((uint32_t)p.rootInventory.size());
    w->u32((uint32_t)p.inventory.size());
    for (uint32_t id : p.rootInventory) w->u32(id);
    for (const MpInventoryNode& node : p.inventory) {
        w->u32(node.id);
        w->i32(node.pid);
        w->i32(node.fid);
        w->i32(node.frame);
        w->i32(node.rotation);
        w->i32(node.flags);
        w->i32(node.dataFlags);
        w->i32(node.quantity);
        w->i32(node.lightDistance);
        w->i32(node.lightIntensity);
        w->i32(node.weaponAmmoQuantity);
        w->i32(node.weaponAmmoTypePid);
        w->i32(node.ammoQuantity);
        w->i32(node.miscCharges);
        w->i32(node.keyCode);
        w->u32((uint32_t)node.children.size());
        for (uint32_t child : node.children) w->u32(child);
    }
}

static bool readProfileSectionModel(Reader* r, MpPlayerProfile* p)
{
    uint32_t modelFileCount;
    if (!r->u32(&p->modelHash) || !r->u32(&modelFileCount)
        || modelFileCount > 512) {
        return false;
    }
    p->modelFiles.clear();
    for (uint32_t index = 0; index < modelFileCount; index++) {
        uint32_t pathLength;
        uint32_t dataLength;
        if (!r->u32(&pathLength) || pathLength == 0 || pathLength > 64
            || !r->u32(&dataLength) || dataLength == 0
            || dataLength > MP_PROFILE_MAX_BYTES / 2
            || pathLength > r->remaining()) {
            return false;
        }
        MpModelFile file;
        file.path.resize(pathLength);
        if (!r->bytes(file.path.data(), pathLength)
            || !isSafeModelFilePath(file.path)
            || dataLength > r->remaining()) {
            return false;
        }
        file.data.resize(dataLength);
        if (!r->bytes(file.data.data(), dataLength)) return false;
        p->modelFiles.push_back(std::move(file));
    }
    return true;
}

static void writeProfileSectionModel(Writer* w, const MpPlayerProfile& p, bool includeModel)
{
    w->u32(p.modelHash);
    // The model payload is omitted when the receiver is known to hold it; the
    // hash always travels so the receiver can resolve or request it.
    w->u32((uint32_t)(includeModel ? p.modelFiles.size() : 0));
    if (includeModel) {
        for (const MpModelFile& file : p.modelFiles) {
            w->u32((uint32_t)file.path.size());
            w->u32((uint32_t)file.data.size());
            w->bytes(file.path.data(), file.path.size());
            w->bytes(file.data.data(), file.data.size());
        }
    }
}

static bool readProfileSectionVolatile(Reader* r, MpPlayerProfile* p)
{
    return r->i32(&p->tile)
        && r->i32(&p->x)
        && r->i32(&p->y)
        && r->i32(&p->sx)
        && r->i32(&p->sy)
        && r->i32(&p->frame)
        && r->i32(&p->rotation)
        && r->i32(&p->fid)
        && r->i32(&p->flags)
        && r->i32(&p->elevation)
        && r->i32(&p->lightDistance)
        && r->i32(&p->lightIntensity)
        && r->i32(&p->hp)
        && r->i32(&p->radiation)
        && r->i32(&p->poison)
        && r->i32(&p->reaction)
        && r->i32(&p->combatManeuver)
        && r->i32(&p->combatAp)
        && r->i32(&p->combatResults)
        && r->i32(&p->combatDamageLastTurn)
        && r->i32(&p->combatAiPacket)
        && r->i32(&p->combatTeam)
        && r->u32(&p->whoHitMeNetId);
}

static void writeProfileSectionVolatile(Writer* w, const MpPlayerProfile& p)
{
    w->i32(p.tile);
    w->i32(p.x);
    w->i32(p.y);
    w->i32(p.sx);
    w->i32(p.sy);
    w->i32(p.frame);
    w->i32(p.rotation);
    w->i32(p.fid);
    w->i32(p.flags);
    w->i32(p.elevation);
    w->i32(p.lightDistance);
    w->i32(p.lightIntensity);
    w->i32(p.hp);
    w->i32(p.radiation);
    w->i32(p.poison);
    w->i32(p.reaction);
    w->i32(p.combatManeuver);
    w->i32(p.combatAp);
    w->i32(p.combatResults);
    w->i32(p.combatDamageLastTurn);
    w->i32(p.combatAiPacket);
    w->i32(p.combatTeam);
    w->u32(p.whoHitMeNetId);
}

// Serializes one section's payload (no section header) into |writer|.
static bool writeSectionBody(Writer* writer, const MpPlayerProfile& profile,
    ProfileSectionId sectionId, bool includeModel)
{
    switch (sectionId) {
    case PROFILE_SECTION_IDENTITY:
        writeProfileSectionIdentity(writer, profile);
        break;
    case PROFILE_SECTION_BASE_STATS:
        writeProfileSectionBaseStats(writer, profile);
        break;
    case PROFILE_SECTION_BONUS_STATS:
        writeProfileSectionBonusStats(writer, profile);
        break;
    case PROFILE_SECTION_SKILLS:
        writeProfileSectionSkills(writer, profile);
        break;
    case PROFILE_SECTION_PC_STATS:
        writeProfileSectionPcStats(writer, profile);
        break;
    case PROFILE_SECTION_TAGS_TRAITS:
        writeProfileSectionTagsTraits(writer, profile);
        break;
    case PROFILE_SECTION_PERKS:
        writeProfileSectionPerks(writer, profile);
        break;
    case PROFILE_SECTION_KILLS:
        writeProfileSectionKills(writer, profile);
        break;
    case PROFILE_SECTION_SKILL_USE:
        writeProfileSectionSkillUse(writer, profile);
        break;
    case PROFILE_SECTION_INVENTORY:
        writeProfileSectionInventory(writer, profile);
        break;
    case PROFILE_SECTION_MODEL:
        writeProfileSectionModel(writer, profile, includeModel);
        break;
    case PROFILE_SECTION_VOLATILE:
        writeProfileSectionVolatile(writer, profile);
        break;
    default:
        return false;
    }
    return true;
}

// Parses one section's payload into |profile|.
static bool readSectionBody(Reader* reader, MpPlayerProfile* profile,
    ProfileSectionId sectionId)
{
    switch (sectionId) {
    case PROFILE_SECTION_IDENTITY:
        return readProfileSectionIdentity(reader, profile);
    case PROFILE_SECTION_BASE_STATS:
        return readProfileSectionBaseStats(reader, profile);
    case PROFILE_SECTION_BONUS_STATS:
        return readProfileSectionBonusStats(reader, profile);
    case PROFILE_SECTION_SKILLS:
        return readProfileSectionSkills(reader, profile);
    case PROFILE_SECTION_PC_STATS:
        return readProfileSectionPcStats(reader, profile);
    case PROFILE_SECTION_TAGS_TRAITS:
        return readProfileSectionTagsTraits(reader, profile);
    case PROFILE_SECTION_PERKS:
        return readProfileSectionPerks(reader, profile);
    case PROFILE_SECTION_KILLS:
        return readProfileSectionKills(reader, profile);
    case PROFILE_SECTION_SKILL_USE:
        return readProfileSectionSkillUse(reader, profile);
    case PROFILE_SECTION_INVENTORY:
        return readProfileSectionInventory(reader, profile);
    case PROFILE_SECTION_MODEL:
        return readProfileSectionModel(reader, profile);
    case PROFILE_SECTION_VOLATILE:
        return readProfileSectionVolatile(reader, profile);
    default:
        return false;
    }
}

static bool validateInventory(const MpPlayerProfile& profile)
{
    if (profile.rootInventory.size() > MP_PROFILE_MAX_INVENTORY_NODES
        || profile.inventory.size() > MP_PROFILE_MAX_INVENTORY_NODES) {
        debugFilePrint("MPROF: inventory validate failed roots=%zu nodes=%zu",
            profile.rootInventory.size(), profile.inventory.size());
        return false;
    }
    std::vector<uint8_t> state(profile.inventory.size(), 0);
    std::function<bool(uint32_t, size_t)> visit = [&](uint32_t id, size_t depth) {
        if (id == 0 || id > profile.inventory.size() || depth > MP_PROFILE_MAX_INVENTORY_DEPTH) {
            debugFilePrint("MPROF: inventory validate visit failed id=%u depth=%zu", id, depth);
            return false;
        }
        if (state[id - 1] == 1) {
            debugFilePrint("MPROF: inventory validate cycle id=%u", id);
            return false;
        }
        if (state[id - 1] == 2) return true;
        const MpInventoryNode& node = profile.inventory[id - 1];
        if (node.id != id || node.pid < 0 || objectTypeFromPid(node.pid) != OBJ_TYPE_ITEM || node.quantity < 1) {
            debugFilePrint("MPROF: inventory validate node bad id=%u pid=0x%X qty=%d", id, node.pid, node.quantity);
            return false;
        }
        state[id - 1] = 1;
        for (uint32_t child : node.children) {
            if (!visit(child, depth + 1)) return false;
        }
        state[id - 1] = 2;
        return true;
    };
    for (uint32_t id : profile.rootInventory) {
        if (!visit(id, 0)) return false;
    }
    for (uint8_t value : state) {
        if (value != 2) {
            debugFilePrint("MPROF: inventory validate unreachable node");
            return false;
        }
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// session model registry (public: header-declared, lives outside the
// anonymous namespace so installModelFiles inside it can call through the
// header declarations without ambiguity)
// ---------------------------------------------------------------------------

static std::unordered_map<uint32_t, std::vector<MpModelFile>> gMpModelRegistry;

void MpModelRegistryRemember(uint32_t modelHash, const std::vector<MpModelFile>& files)
{
    if (modelHash == 0 || files.empty()) {
        return;
    }
    gMpModelRegistry[modelHash] = files;
    debugFilePrint("MPROF: model registry remember hash=%08X files=%zu entries=%zu",
        modelHash, files.size(), gMpModelRegistry.size());
}

bool MpModelRegistryResolve(uint32_t modelHash, std::vector<MpModelFile>* out)
{
    if (out == nullptr) {
        return false;
    }
    auto it = gMpModelRegistry.find(modelHash);
    if (it == gMpModelRegistry.end()) {
        return false;
    }
    *out = it->second;
    return true;
}

void MpModelRegistryClear()
{
    if (!gMpModelRegistry.empty()) {
        debugFilePrint("MPROF: model registry clear entries=%zu", gMpModelRegistry.size());
        gMpModelRegistry.clear();
    }
}

bool MpProfileResolveModel(MpPlayerProfile* profile)
{
    if (profile == nullptr || profile->modelHash == 0) {
        // No model needed.
        return true;
    }
    if (!profile->modelFiles.empty()) {
        // Payload already present.
        return true;
    }
    if (profile->modelName[0] != '\0') {
        // Vanilla art: resolvable locally without the payload.
        if (artListIndex(OBJ_TYPE_CRITTER, profile->modelName) >= 0) {
            return true;
        }
    }
    return MpModelRegistryResolve(profile->modelHash, &profile->modelFiles);
}

bool MpProfileCaptureObject(const Object* object, MpPlayerProfile* profile)
{
    if (profile == nullptr || !captureCommon(object, profile)) {
        debugFilePrint("MPROF: capture object failed obj=%p", (void*)object);
        return false;
    }
    profile->rootInventory.clear();
    profile->inventory.clear();
    std::unordered_map<const Object*, uint32_t> seen;
    for (int index = 0; index < object->data.inventory.length; index++) {
        const InventoryItem* item = &object->data.inventory.items[index];
        if (item->item == nullptr || item->quantity < 1) {
            debugFilePrint("MPROF: capture object bad root item idx=%d item=%p qty=%d",
                index, (void*)item->item, item->quantity);
            return false;
        }
        uint32_t id = 0;
        if (!captureInventoryNode(item->item, profile, &seen, &id, 0)) return false;
        profile->inventory[id - 1].quantity = item->quantity;
        profile->rootInventory.push_back(id);
    }
    if (!MpProfileValidate(*profile)) {
        debugFilePrint("MPROF: capture object validate failed name='%s' nodes=%zu roots=%zu",
            profile->name, profile->inventory.size(), profile->rootInventory.size());
        return false;
    }
    debugFilePrint("MPROF: capture object done name='%s' pid=0x%X tile=%d nodes=%zu roots=%zu",
        profile->name, object->pid, object->tile, profile->inventory.size(), profile->rootInventory.size());
    return true;
}

bool MpProfileCaptureObjectNoModel(const Object* object, MpPlayerProfile* profile)
{
    if (profile == nullptr || !captureCommon(object, profile)) {
        debugFilePrint("MPROF: capture object(no-model) failed obj=%p", (void*)object);
        return false;
    }
    profile->rootInventory.clear();
    profile->inventory.clear();
    std::unordered_map<const Object*, uint32_t> seen;
    for (int index = 0; index < object->data.inventory.length; index++) {
        const InventoryItem* item = &object->data.inventory.items[index];
        if (item->item == nullptr || item->quantity < 1) {
            debugFilePrint("MPROF: capture object(no-model) bad root item idx=%d qty=%d", index, item->quantity);
            return false;
        }
        uint32_t id = 0;
        if (!captureInventoryNode(item->item, profile, &seen, &id, 0)) return false;
        profile->inventory[id - 1].quantity = item->quantity;
        profile->rootInventory.push_back(id);
    }
    profile->modelFiles.clear();
    profile->modelHash = 0;
    if (!MpProfileValidate(*profile)) return false;
    // NOTE: no success log here on purpose — this runs every host tick from
    // mpHostSyncProfiles; success logging would spam the log file.
    return true;
}

bool MpProfileCaptureLocalNoModel(MpPlayerProfile* profile)
{
    if (profile == nullptr || gDude == nullptr) {
        debugFilePrint("MPROF: capture local(no-model) failed dude=%p", (void*)gDude);
        return false;
    }
    if (!MpProfileCaptureObjectNoModel(gDude, profile)) return false;
    for (int index = 0; index < PC_STAT_COUNT; index++) {
        profile->pcStats[index] = pcGetStat((PcStat)index);
    }
    skillsGetTagged((Skill*)profile->taggedSkills, NUM_TAGGED_SKILLS);
    traitsGetSelected((Trait*)&profile->selectedTraits[0], (Trait*)&profile->selectedTraits[1]);
    perksGetRanks(profile->perkRanks, PERK_COUNT);
    killsGetAll(profile->killCounts, KILL_TYPE_DEFAULT_COUNT);
    profile->sneakWorking = critterGetSneakWorking();
    profile->remainingCharacterPoints = gCharacterEditorRemainingCharacterPoints;
    // NOTE: no success log here on purpose — this runs every host tick from
    // mpHostSyncProfiles; success logging would spam the log file.
    return true;
}

bool MpProfileCaptureLocal(MpPlayerProfile* profile)
{
    if (!MpProfileCaptureLocalNoModel(profile)) return false;
    if (!captureModelFiles(profile)) {
        debugFilePrint("MPROF: capture local model capture failed name='%s'", profile->name);
        return false;
    }
    debugFilePrint("MPROF: capture local done name='%s' nodes=%zu roots=%zu models=%zu hash=%08X",
        profile->name, profile->inventory.size(), profile->rootInventory.size(),
        profile->modelFiles.size(), profile->modelHash);
    return true;
}

bool MpProfileValidate(const MpPlayerProfile& profile)
{
    if (profile.schemaVersion != MP_PROFILE_SCHEMA_VERSION) {
        debugFilePrint("MPROF: validate failed schema=%u want=%u", profile.schemaVersion, MP_PROFILE_SCHEMA_VERSION);
        return false;
    }
    if (profile.name[MP_PROFILE_NAME_LENGTH - 1] != '\0') {
        debugFilePrint("MPROF: validate failed name not terminated");
        return false;
    }
    if (profile.inventory.size() > MP_PROFILE_MAX_INVENTORY_NODES) {
        debugFilePrint("MPROF: validate failed inventory nodes=%zu cap=%zu", profile.inventory.size(), MP_PROFILE_MAX_INVENTORY_NODES);
        return false;
    }
    if (profile.rootInventory.size() > profile.inventory.size()) {
        debugFilePrint("MPROF: validate failed roots=%zu nodes=%zu", profile.rootInventory.size(), profile.inventory.size());
        return false;
    }
    if (!hexGridTileIsValid(profile.tile) && profile.tile != -1) {
        debugFilePrint("MPROF: validate failed tile=%d", profile.tile);
        return false;
    }
    if (!elevationIsValid(profile.elevation)) {
        debugFilePrint("MPROF: validate failed elevation=%d", profile.elevation);
        return false;
    }
    if (memchr(profile.modelName, '\0', sizeof(profile.modelName)) == nullptr) {
        debugFilePrint("MPROF: validate failed modelName not terminated");
        return false;
    }
    if (profile.modelFiles.size() > 512) {
        debugFilePrint("MPROF: validate failed model files=%zu cap=512", profile.modelFiles.size());
        return false;
    }
    size_t modelBytes = 0;
    std::unordered_set<std::string> modelPaths;
    for (const MpModelFile& file : profile.modelFiles) {
        if (!isSafeModelFilePath(file.path)) {
            debugFilePrint("MPROF: validate failed unsafe model path '%s'", file.path.c_str());
            return false;
        }
        if (file.data.empty() || modelPaths.find(file.path) != modelPaths.end()
            || file.data.size() > MP_PROFILE_MAX_BYTES / 2) {
            debugFilePrint("MPROF: validate failed model file dup/empty/oversize '%s' bytes=%zu",
                file.path.c_str(), file.data.size());
            return false;
        }
        modelPaths.insert(file.path);
        modelBytes += file.data.size();
        if (modelBytes > MP_PROFILE_MAX_BYTES / 2) {
            debugFilePrint("MPROF: validate failed model total bytes=%zu cap=%zu", modelBytes, MP_PROFILE_MAX_BYTES / 2);
            return false;
        }
    }
    if (!profile.modelFiles.empty() && hashModelFiles(profile.modelFiles) != profile.modelHash) {
        debugFilePrint("MPROF: validate failed model hash mismatch files=%zu hash=%08X want=%08X",
            profile.modelFiles.size(), hashModelFiles(profile.modelFiles), profile.modelHash);
        return false;
    }
    if (profile.bodyType < BODY_TYPE_FIRST || profile.bodyType >= BODY_TYPE_COUNT) {
        debugFilePrint("MPROF: validate failed bodyType=%d", profile.bodyType);
        return false;
    }
    if (profile.damageType < DAMAGE_TYPE_FIRST || profile.damageType >= DAMAGE_TYPE_COUNT) {
        debugFilePrint("MPROF: validate failed damageType=%d", profile.damageType);
        return false;
    }
    if (profile.experience < 0) {
        debugFilePrint("MPROF: validate failed experience=%d", profile.experience);
        return false;
    }
    if (profile.pcStats[PC_STAT_LEVEL] < 0 || profile.pcStats[PC_STAT_LEVEL] > PC_LEVEL_MAX) {
        debugFilePrint("MPROF: validate failed level=%d", profile.pcStats[PC_STAT_LEVEL]);
        return false;
    }
    for (int skill : profile.taggedSkills) {
        if (skill != SKILL_INVALID && !skillIsValid(skill)) {
            debugFilePrint("MPROF: validate failed tagged skill=%d", skill);
            return false;
        }
    }
    for (int trait : profile.selectedTraits) {
        if (trait < -1 || trait >= TRAIT_COUNT) {
            debugFilePrint("MPROF: validate failed trait=%d", trait);
            return false;
        }
    }
    for (int rank : profile.perkRanks) {
        if (rank < 0 || rank > 255) {
            debugFilePrint("MPROF: validate failed perk rank=%d", rank);
            return false;
        }
    }
    return validateInventory(profile);
}

uint32_t MpProfileHash(const MpPlayerProfile& profile)
{
    std::vector<uint8_t> data;
    MpPlayerProfile copy = profile;
    // Only the generation is excluded from the content hash (it lives in the
    // packet header, not the serialized body). The model hash must be kept:
    // zeroing it while modelFiles are present makes MpProfileValidate fail and
    // collapses every content hash to 0.
    copy.generation = 0;
    if (!MpProfileSerialize(copy, &data)) return 0;
    return hashBytes(data.data(), data.size());
}

bool MpProfileSerialize(const MpPlayerProfile& profile, std::vector<uint8_t>* data,
    bool includeModel)
{
    if (data == nullptr || !MpProfileValidate(profile)) return false;
    std::vector<uint8_t> body;
    NetProfileSectionInfo infos[PROFILE_SECTION_COUNT];
    uint16_t sectionCount = 0;
    uint32_t crc = 0;
    if (!MpProfileBuildBody(profile, /*changedSections=*/0, includeModel,
            &body, infos, &sectionCount, &crc)) {
        return false;
    }
    if (body.size() > MP_PROFILE_MAX_BYTES - kProfileHeaderSize) return false;

    Writer output;
    output.u32(kProfileMagic);
    output.u16(profile.schemaVersion);
    output.u16(0);
    output.u32(profile.generation);
    output.u32((uint32_t)body.size());
    output.u32(crc);
    output.bytes(body.data(), body.size());
    *data = output.data();
    return true;
}

bool MpProfileDeserialize(const void* data, size_t dataLength, MpPlayerProfile* profile)
{
    if (data == nullptr || profile == nullptr || dataLength < kProfileHeaderSize
        || dataLength > MP_PROFILE_MAX_BYTES) return false;
    Reader reader(data, dataLength);
    uint32_t magic;
    uint16_t schema;
    uint16_t reserved;
    uint32_t generation;
    uint32_t bodyLength;
    uint32_t expectedHash;
    if (!reader.u32(&magic) || !reader.u16(&schema) || !reader.u16(&reserved)
        || !reader.u32(&generation) || !reader.u32(&bodyLength) || !reader.u32(&expectedHash)
        || magic != kProfileMagic || schema != MP_PROFILE_SCHEMA_VERSION
        || bodyLength != reader.remaining() || bodyLength > MP_PROFILE_MAX_BYTES - kProfileHeaderSize) {
        return false;
    }
    std::vector<uint8_t> body(bodyLength);
    if (!reader.bytes(body.data(), body.size())
        || hashBytes(body.data(), body.size()) != expectedHash) return false;
    MpPlayerProfile candidate;
    candidate.schemaVersion = schema;
    candidate.generation = generation;
    uint32_t mask = MpProfileDeserializeSections(body.data(), body.size(), &candidate);
    if (mask == 0 || !MpProfileValidate(candidate)) return false;
    *profile = std::move(candidate);
    return true;
}

bool MpProfileSerializeSection(const MpPlayerProfile& profile, uint8_t sectionId,
    bool includeModel, std::vector<uint8_t>* out)
{
    if (out == nullptr || sectionId < PROFILE_SECTION_IDENTITY
        || sectionId > PROFILE_SECTION_COUNT) {
        return false;
    }
    Writer writer;
    if (!writeSectionBody(&writer, profile, (ProfileSectionId)sectionId, includeModel)) {
        return false;
    }
    *out = writer.data();
    return true;
}

uint32_t MpProfileDeserializeSections(const void* data, size_t dataLength,
    MpPlayerProfile* profile)
{
    if (data == nullptr || profile == nullptr) {
        return 0;
    }
    Reader reader(data, dataLength);
    uint32_t mask = 0;
    while (reader.remaining() > 0) {
        uint8_t id = 0;
        uint8_t reserved = 0;
        uint32_t size = 0;
        size_t headerStart = dataLength - reader.remaining();
        if (!reader.u8(&id) || !reader.u8(&reserved) || !reader.u32(&size)
            || id < PROFILE_SECTION_IDENTITY || id > PROFILE_SECTION_COUNT
            || size > reader.remaining()) {
            debugFilePrint("MPROF: deserialize bad header offset=%zu remaining=%zu id=%u size=%u",
                headerStart, reader.remaining(), (unsigned)id, (unsigned)size);
            return 0;
        }
        // The Reader does not expose its offset — derive the payload start
        // from the bytes remaining and construct a bounded sub-reader.
        size_t payloadStart = dataLength - reader.remaining();
        Reader section((const uint8_t*)data + payloadStart, size);
        if (!readSectionBody(&section, profile, (ProfileSectionId)id)) {
            debugFilePrint("MPROF: deserialize section body failed id=%u size=%u offset=%zu remaining=%zu",
                (unsigned)id, (unsigned)size, payloadStart, reader.remaining());
            return 0;
        }
        // The payload was consumed by the sub-reader — advance the parent
        // past it or the next header read lands inside this payload.
        if (!reader.skip(size)) {
            debugFilePrint("MPROF: deserialize section skip failed id=%u size=%u",
                (unsigned)id, (unsigned)size);
            return 0;
        }
        mask |= (1u << (id - 1));
    }
    return mask;
}

bool MpProfileSectionChanged(const MpPlayerProfile& a, const MpPlayerProfile& b,
    uint8_t sectionId)
{
    if (sectionId == PROFILE_SECTION_VOLATILE) {
        // Volatile fields are owned by the per-tick state channel — never
        // change-detected (and never sent by the detection loops).
        return false;
    }
    if (sectionId == PROFILE_SECTION_MODEL) {
        // Model identity: change-detection captures are model-file-free, so
        // their file hash is always 0 — the model NAME is the identity signal
        // (the payload itself is content-addressed by the real hash).
        return a.modelHash != b.modelHash
            || strncmp(a.modelName, b.modelName, sizeof(a.modelName)) != 0;
    }
    std::vector<uint8_t> sectionA;
    std::vector<uint8_t> sectionB;
    if (!MpProfileSerializeSection(a, sectionId, /*includeModel=*/false, &sectionA)
        || !MpProfileSerializeSection(b, sectionId, /*includeModel=*/false, &sectionB)) {
        return true; // cannot serialize — assume changed
    }
    return sectionA != sectionB;
}

bool MpProfileBuildBody(const MpPlayerProfile& profile, uint32_t changedSections,
    bool includeModel, std::vector<uint8_t>* body,
    NetProfileSectionInfo* infos, uint16_t* sectionCount, uint32_t* contentHash)
{
    if (body == nullptr || infos == nullptr || sectionCount == nullptr
        || contentHash == nullptr) {
        return false;
    }
    Writer writer;
    uint16_t count = 0;
    for (int id = PROFILE_SECTION_IDENTITY; id <= PROFILE_SECTION_COUNT; id++) {
        if (changedSections != 0 && (changedSections & (1u << (id - 1))) == 0) {
            continue;
        }
        std::vector<uint8_t> payload;
        bool modelPayload = (id == PROFILE_SECTION_MODEL) && includeModel;
        if (!MpProfileSerializeSection(profile, (uint8_t)id, modelPayload, &payload)) {
            return false;
        }
        infos[count].id = (uint8_t)id;
        infos[count].reserved = 0;
        infos[count].reserved2 = 0;
        infos[count].byteSize = (uint32_t)payload.size();
        writer.u8((uint8_t)id);
        writer.u8(0);
        writer.u32((uint32_t)payload.size());
        writer.bytes(payload.data(), payload.size());
        count++;
    }
    *body = writer.data();
    *sectionCount = count;
    *contentHash = hashBytes(body->data(), body->size());
    return true;
}

uint32_t MpProfileHashBytes(const void* data, size_t size)
{
    return hashBytes(data, size);
}

MpPlayerRuntime* MpProfileCreateRuntime(uint8_t netId, const MpPlayerProfile& profile,
    int tile, int elevation, int rotation)
{
    debugFilePrint("MPROF: create runtime begin netId=%u name='%s' gen=%u tile=%d elev=%d rot=%d",
        netId, profile.name, profile.generation, tile, elevation, rotation);
    if (netId == 0 || !MpProfileValidate(profile)) {
        debugFilePrint("MPROF: create runtime failed validate netId=%u", netId);
        return nullptr;
    }
    MpPlayerProfile resolvedProfile = profile;
    if (!installModelFiles(&resolvedProfile)) {
        debugFilePrint("MPROF: create runtime failed install model netId=%u", netId);
        return nullptr;
    }
    MpProfileDestroyRuntime(netId);

    int pid;
    if (proto_new(&pid, OBJ_TYPE_CRITTER) != 0) {
        debugFilePrint("MPROF: create runtime failed proto_new netId=%u", netId);
        return nullptr;
    }
    Proto* source = nullptr;
    Proto* target = nullptr;
    if (protoGetProto(0x1000000, &source) != 0 || protoGetProto(pid, &target) != 0
        || source == nullptr || target == nullptr) {
        debugFilePrint("MPROF: create runtime failed protoGetProto netId=%u pid=%d", netId, pid);
        protoRemove(pid);
        return nullptr;
    }
    target->critter = source->critter;
    target->critter.pid = pid;
    target->critter.messageId = resolvedProfile.prototypeMessageId;
    target->critter.fid = resolvedProfile.prototypeFid;
    target->critter.lightDistance = resolvedProfile.prototypeLightDistance;
    target->critter.lightIntensity = resolvedProfile.prototypeLightIntensity;
    target->critter.flags = resolvedProfile.prototypeFlags;
    target->critter.extendedFlags = resolvedProfile.prototypeExtendedFlags;
    target->critter.sid = -1;
    target->critter.data.flags = resolvedProfile.critterFlags;
    memcpy(target->critter.data.baseStats, resolvedProfile.baseStats, sizeof(resolvedProfile.baseStats));
    memcpy(target->critter.data.bonusStats, resolvedProfile.bonusStats, sizeof(resolvedProfile.bonusStats));
    memcpy(target->critter.data.skills, resolvedProfile.skills, sizeof(resolvedProfile.skills));
    target->critter.data.bodyType = (BodyType)resolvedProfile.bodyType;
    target->critter.data.experience = resolvedProfile.experience;
    target->critter.data.killType = (KillType)resolvedProfile.killType;
    target->critter.data.damageType = (DamageType)resolvedProfile.damageType;
    target->critter.headFid = resolvedProfile.prototypeHeadFid;
    target->critter.aiPacket = resolvedProfile.prototypeAiPacket;
    target->critter.team = resolvedProfile.prototypeTeam;

    Object* object = nullptr;
    int fid = resolvedProfile.fid != 0 ? resolvedProfile.fid : target->critter.fid;
    if (objectCreateWithFidPid(&object, fid, pid) != 0 || object == nullptr) {
        debugFilePrint("MPROF: create runtime failed objectCreate netId=%u fid=0x%X pid=0x%X",
            netId, fid, pid);
        protoRemove(pid);
        return nullptr;
    }
    object->flags |= OBJECT_NO_REMOVE | OBJECT_NO_SAVE;
    object->sid = -1;
    object->scriptIndex = -1;
    object->x = resolvedProfile.x;
    object->y = resolvedProfile.y;
    object->sx = resolvedProfile.sx;
    object->sy = resolvedProfile.sy;
    object->frame = resolvedProfile.frame;
    object->flags = resolvedProfile.flags | OBJECT_NO_REMOVE | OBJECT_NO_SAVE;
    object->lightDistance = resolvedProfile.lightDistance;
    object->lightIntensity = resolvedProfile.lightIntensity;
    object->data.critter.hp = resolvedProfile.hp;
    object->data.critter.radiation = resolvedProfile.radiation;
    object->data.critter.poison = resolvedProfile.poison;
    object->data.critter.reaction = resolvedProfile.reaction;
    object->data.critter.combat.maneuver = resolvedProfile.combatManeuver;
    object->data.critter.combat.ap = resolvedProfile.combatAp;
    object->data.critter.combat.results = resolvedProfile.combatResults;
    object->data.critter.combat.damageLastTurn = resolvedProfile.combatDamageLastTurn;
    object->data.critter.combat.aiPacket = resolvedProfile.combatAiPacket;
    object->data.critter.combat.team = resolvedProfile.combatTeam;
    object->data.critter.combat.whoHitMe = nullptr;
    if (hexGridTileIsValid(tile) && elevationIsValid(elevation)) {
        objectSetLocation(object, tile, elevation, nullptr);
    }
    objectSetRotation(object, static_cast<Rotation>(rotation), nullptr);

    std::unordered_map<uint32_t, Object*> built;
    for (uint32_t root : resolvedProfile.rootInventory) {
        if (!applyItemNode(resolvedProfile, root, object, &built, 0)) {
            debugFilePrint("MPROF: create runtime failed inventory apply netId=%u root=%u",
                netId, root);
            object->flags &= ~OBJECT_NO_REMOVE;
            objectDestroy(object, nullptr);
            protoRemove(pid);
            return nullptr;
        }
    }
    object->fid = resolvedProfile.fid;
    object->frame = resolvedProfile.frame;
    // Placement (tile/elevation/rotation) comes from the caller's spawn
    // arguments — the host-anchored join spawn or the map-change spawn — and
    // was already applied above. The profile's transform fields are the
    // player's last-known solo position, which may be a DIFFERENT map or a
    // different elevation; letting them overwrite the spawn would put the
    // avatar on the wrong elevation and make it invisible to the host.

    MpPlayerRuntime runtime;
    runtime.profile = resolvedProfile;
    runtime.object = object;
    runtime.syntheticPid = pid;
    auto inserted = gRuntimes.emplace(netId, std::move(runtime));
    if (!inserted.second) {
        debugFilePrint("MPROF: create runtime failed duplicate netId=%u", netId);
        object->flags &= ~OBJECT_NO_REMOVE;
        objectDestroy(object, nullptr);
        protoRemove(pid);
        return nullptr;
    }
    gObjectToRuntime[object] = netId;
    objectShow(object, nullptr);
    objectReorder(object);
    debugFilePrint("MPROF: create runtime done netId=%u name='%s' obj=%p pid=0x%X tile=%d items=%zu",
        netId, resolvedProfile.name, (void*)object, pid, object->tile, built.size());
    return &inserted.first->second;
}

bool MpProfileBindLocal(uint8_t netId, const MpPlayerProfile& profile, Object* object)
{
    debugFilePrint("MPROF: bind local begin netId=%u name='%s' gen=%u obj=%p",
        netId, profile.name, profile.generation, (void*)object);
    if (netId == 0 || object == nullptr || !MpProfileValidate(profile)) {
        debugFilePrint("MPROF: bind local failed validate netId=%u obj=%p", netId, (void*)object);
        return false;
    }
    MpProfileDestroyRuntime(netId);
    MpPlayerRuntime runtime;
    runtime.profile = profile;
    runtime.object = object;
    runtime.syntheticPid = -1;
    auto inserted = gRuntimes.emplace(netId, std::move(runtime));
    if (!inserted.second) {
        debugFilePrint("MPROF: bind local failed duplicate netId=%u", netId);
        return false;
    }
    gObjectToRuntime[object] = netId;
    if (gMpIsClient && netId > 0 && netId <= NET_MAX_PLAYERS) {
        // The local dude's reverse-map entry must survive the rebind: the
        // destroy above scrubbed nothing (gDude exempt), but re-registering
        // here also heals any earlier wipe (e.g. a map-change cleanup that
        // ran before the slot's obj was set).
        MultiplayerPlayer* p = &gMpSession.players[netId - 1];
        if (p->isConnected && p->objNetId != 0) {
            MpRegisterObjNetId(object, p->objNetId);
        }
    }
    debugFilePrint("MPROF: bind local done netId=%u name='%s'", netId, profile.name);
    return true;
}

void MpProfileUpdateRuntime(uint8_t netId, const MpPlayerProfile& profile)
{
    auto it = gRuntimes.find(netId);
    if (it == gRuntimes.end()) {
        debugFilePrint("MPROF: update runtime no runtime netId=%u name='%s'", netId, profile.name);
        return;
    }
    MpPlayerRuntime& runtime = it->second;
    if (runtime.object == nullptr) {
        // Avatar detached (mid map change); keep the old profile as the
        // authoritative record until the avatar is rebuilt.
        debugFilePrint("MPROF: update runtime avatar detached netId=%u gen=%u", netId, profile.generation);
        return;
    }
    MpPlayerProfile updated = profile;
    if (runtime.profile.modelName[0] != '\0'
        && strncmp(runtime.profile.modelName, profile.modelName,
            sizeof(runtime.profile.modelName)) == 0) {
        // Same model identity: preserve the installed model payload and the
        // process-local resolved index instead of re-transferring files.
        updated.modelFiles = runtime.profile.modelFiles;
        updated.modelHash = runtime.profile.modelHash;
        updated.localModelIndex = runtime.profile.localModelIndex;
    } else {
        // Model identity changed (e.g. armor swap): install the new model.
        MpPlayerProfile resolved = profile;
        if (!installModelFiles(&resolved)) {
            debugFilePrint("MPROF: update runtime failed install model netId=%u name='%s'",
                netId, profile.modelName);
            return;
        }
        updated = std::move(resolved);
    }
    runtime.profile = std::move(updated);
    debugFilePrint("MPROF: update runtime done netId=%u name='%s' gen=%u modelIdx=%d",
        netId, runtime.profile.name, runtime.profile.generation, runtime.profile.localModelIndex);
}

// Re-applies a runtime's profile onto its live avatar object: proto data
// (stats, skills, flags, experience), the synthetic proto's identity fields,
// and the inventory graph. Transform/HP/combat fields are intentionally NOT
// touched — they live on the per-tick player/object state channel. Called on
// the host after a client profile update.
static bool mpProfileReapplyAvatar(MpPlayerRuntime& runtime, uint32_t changedSections)
{
    Object* object = runtime.object;
    if (object == nullptr) {
        return false;
    }
    Proto* proto = nullptr;
    if (protoGetProto(runtime.syntheticPid, &proto) == -1 || proto == nullptr) {
        debugFilePrint("MPROF: reapply avatar protoGetProto failed pid=%d", runtime.syntheticPid);
        return false;
    }

    const MpPlayerProfile& profile = runtime.profile;
    // Only the sections present on the wire are written; changedSections == 0
    // means "apply everything" (join transfers).
    if (changedSections == 0
        || (changedSections & (1u << (PROFILE_SECTION_IDENTITY - 1))) != 0) {
        proto->critter.messageId = profile.prototypeMessageId;
        proto->critter.fid = profile.prototypeFid;
        proto->critter.lightDistance = profile.prototypeLightDistance;
        proto->critter.lightIntensity = profile.prototypeLightIntensity;
        proto->critter.flags = profile.prototypeFlags;
        proto->critter.extendedFlags = profile.prototypeExtendedFlags;
        proto->critter.data.flags = profile.critterFlags;
        proto->critter.data.bodyType = (BodyType)profile.bodyType;
        proto->critter.data.experience = profile.experience;
        proto->critter.data.killType = (KillType)profile.killType;
        proto->critter.data.damageType = (DamageType)profile.damageType;
        proto->critter.headFid = profile.prototypeHeadFid;
        proto->critter.aiPacket = profile.prototypeAiPacket;
        proto->critter.team = profile.prototypeTeam;
        // The object's own fields derived from the proto at creation.
        object->lightDistance = profile.lightDistance;
        object->lightIntensity = profile.lightIntensity;
    }
    if (changedSections == 0
        || (changedSections & (1u << (PROFILE_SECTION_BASE_STATS - 1))) != 0) {
        memcpy(proto->critter.data.baseStats, profile.baseStats, sizeof(profile.baseStats));
    }
    if (changedSections == 0
        || (changedSections & (1u << (PROFILE_SECTION_BONUS_STATS - 1))) != 0) {
        memcpy(proto->critter.data.bonusStats, profile.bonusStats, sizeof(profile.bonusStats));
    }
    if (changedSections == 0
        || (changedSections & (1u << (PROFILE_SECTION_SKILLS - 1))) != 0) {
        memcpy(proto->critter.data.skills, profile.skills, sizeof(profile.skills));
    }
    if (changedSections == 0
        || (changedSections & (1u << (PROFILE_SECTION_INVENTORY - 1))) != 0) {
        // Rebuild the inventory graph from the canonical profile. The old
        // items are detached and destroyed (children first) like
        // MpProfileDetachAvatar.
        mpProfileDestroyObjectItems(object);
        object->data.inventory.length = 0;
        std::unordered_map<uint32_t, Object*> built;
        for (uint32_t root : profile.rootInventory) {
            if (!applyItemNode(profile, root, object, &built, 0)) {
                debugFilePrint("MPROF: reapply avatar inventory failed root=%u", root);
                return false;
            }
        }
    }
    objectReorder(object);
    return true;
}

bool MpProfileApplyRuntimeUpdate(uint8_t netId, const MpPlayerProfile& profile,
    uint32_t changedSections)
{
    auto it = gRuntimes.find(netId);
    if (it == gRuntimes.end() || it->second.object == nullptr) {
        debugFilePrint("MPROF: apply runtime update no avatar netId=%u name='%s'",
            netId, profile.name);
        return false;
    }
    // Update the stored profile first (model-aware); then re-apply the sheet
    // onto the live avatar. The object itself is never recreated, so the
    // combat list / netId mappings / player->obj stay valid.
    MpProfileUpdateRuntime(netId, profile);
    it = gRuntimes.find(netId);
    if (it == gRuntimes.end() || it->second.object == nullptr) {
        return false;
    }
    // Level-ups raise STAT_MAXIMUM_HIT_POINTS (bonusStats) and heal the
    // character by the same amount (vanilla pcAddExperience). The avatar's
    // current HP is host-authoritative and never rides the profile, so the
    // gain must be mirrored here — otherwise the state sync reverts the
    // client's level-up heal and its HP oscillates.
    int maxHpBefore = critterGetStat(it->second.object, STAT_MAXIMUM_HIT_POINTS);
    if (!mpProfileReapplyAvatar(it->second, changedSections)) {
        debugFilePrint("MPROF: apply runtime update reapply failed netId=%u", netId);
        return false;
    }
    // The maxHp-before/after comparison is the detection — it is self-limiting
    // (no delta, no heal) and must NOT be mask-gated: the client's level-up
    // upload can legitimately carry only PC_STATS (XP/level) while the max-HP
    // bonus already rode an earlier section, and the gate would skip the heal
    // and leave the avatar's HP behind the client's.
    {
        int maxHpAfter = critterGetStat(it->second.object, STAT_MAXIMUM_HIT_POINTS);
        if (maxHpAfter > maxHpBefore) {
            critterAdjustHitPoints(it->second.object, maxHpAfter - maxHpBefore);
            debugFilePrint("MPROF: level-up hp mirrored netId=%u maxHp=%d->%d hp=%d",
                netId, maxHpBefore, maxHpAfter,
                critterGetHitPoints(it->second.object));
        }
    }
    debugFilePrint("MPROF: apply runtime update done netId=%u name='%s' gen=%u items=%zu sections=%08X",
        netId, it->second.profile.name, it->second.profile.generation,
        it->second.profile.rootInventory.size(), changedSections);
    // The avatar's inventory was rebuilt from the profile — the weapon slot of
    // its FID never went through inven_wield, so re-derive it from the hands.
    MpSyncCritterWeaponFid(it->second.object);
    return true;
}

void MpProfileGrantCombatXp(int xp)
{
    if (xp <= 0 || !gMpIsHost || !gMpActive) {
        return;
    }
    for (int index = 0; index < NET_MAX_PLAYERS; index++) {
        MultiplayerPlayer* player = &gMpSession.players[index];
        if (!player->isConnected || player->isLocal || player->obj == nullptr) {
            continue;
        }
        MpPlayerRuntime* runtime = MpProfileGetRuntime(player->netId);
        if (runtime == nullptr || runtime->object == nullptr) {
            continue;
        }
        Proto* proto = nullptr;
        if (protoGetProto(runtime->syntheticPid, &proto) == -1 || proto == nullptr) {
            continue;
        }
        proto->critter.data.experience += xp;
        debugFilePrint("MPROF: combat xp granted netId=%u xp=%d total=%d",
            player->netId, xp, proto->critter.data.experience);
    }
}

bool MpProfileApplyLocal(const MpPlayerProfile& profile, bool applyPcStats,
    uint32_t changedSections)
{
    debugFilePrint("MPROF: apply local begin name='%s' gen=%u nodes=%zu roots=%zu applyPcStats=%d sections=%08X",
        profile.name, profile.generation, profile.inventory.size(), profile.rootInventory.size(),
        applyPcStats ? 1 : 0, changedSections);
    if (!MpProfileValidate(profile) || gDude == nullptr) {
        debugFilePrint("MPROF: apply local failed validate/dude");
        return false;
    }
    Proto* proto = nullptr;
    if (protoGetProto(gDude->pid, &proto) == -1 || proto == nullptr
        || objectTypeFromPid(gDude->pid) != OBJ_TYPE_CRITTER) {
        debugFilePrint("MPROF: apply local failed protoGetProto pid=0x%X", gDude->pid);
        return false;
    }

    // Only the sections present on the wire are written; changedSections == 0
    // means "apply everything" (join transfers).
    if (changedSections == 0
        || (changedSections & (1u << (PROFILE_SECTION_IDENTITY - 1))) != 0) {
        proto->critter.data.flags = profile.critterFlags;
        proto->critter.data.bodyType = (BodyType)profile.bodyType;
        proto->critter.data.experience = profile.experience;
        proto->critter.data.killType = (KillType)profile.killType;
        proto->critter.data.damageType = (DamageType)profile.damageType;
        proto->critter.flags = profile.prototypeFlags;
        proto->critter.extendedFlags = profile.prototypeExtendedFlags;
        proto->critter.lightDistance = profile.prototypeLightDistance;
        proto->critter.lightIntensity = profile.prototypeLightIntensity;
        proto->critter.headFid = profile.prototypeHeadFid;
        proto->critter.aiPacket = profile.prototypeAiPacket;
        proto->critter.team = profile.prototypeTeam;
        dudeSetName(profile.name);
    }
    if (changedSections == 0
        || (changedSections & (1u << (PROFILE_SECTION_BASE_STATS - 1))) != 0) {
        memcpy(proto->critter.data.baseStats, profile.baseStats, sizeof(profile.baseStats));
    }
    if (changedSections == 0
        || (changedSections & (1u << (PROFILE_SECTION_BONUS_STATS - 1))) != 0) {
        memcpy(proto->critter.data.bonusStats, profile.bonusStats, sizeof(profile.bonusStats));
    }
    if (changedSections == 0
        || (changedSections & (1u << (PROFILE_SECTION_SKILLS - 1))) != 0) {
        memcpy(proto->critter.data.skills, profile.skills, sizeof(profile.skills));
    }
    // The volatile runtime fields ride the per-tick state channel; they only
    // appear in join transfers (changedSections == 0 or the VOLATILE bit).
    if (changedSections == 0
        || (changedSections & (1u << (PROFILE_SECTION_VOLATILE - 1))) != 0) {
        gDude->data.critter.hp = profile.hp;
        gDude->data.critter.radiation = profile.radiation;
        gDude->data.critter.poison = profile.poison;
        gDude->data.critter.reaction = profile.reaction;
        gDude->data.critter.combat.maneuver = profile.combatManeuver;
        gDude->data.critter.combat.ap = profile.combatAp;
        gDude->data.critter.combat.results = profile.combatResults;
        gDude->data.critter.combat.damageLastTurn = profile.combatDamageLastTurn;
        gDude->data.critter.combat.aiPacket = profile.combatAiPacket;
        gDude->data.critter.combat.team = profile.combatTeam;
        gDude->data.critter.combat.whoHitMe = nullptr;
    }

    if (changedSections == 0
        || (changedSections & (1u << (PROFILE_SECTION_PC_STATS - 1))) != 0) {
        for (int index = 0; index < PC_STAT_COUNT; index++) {
            if (applyPcStats) {
                pcSetStat((PcStat)index, profile.pcStats[index]);
            }
        }
    }
    if (changedSections == 0
        || (changedSections & (1u << (PROFILE_SECTION_TAGS_TRAITS - 1))) != 0) {
        skillsSetTagged((Skill*)profile.taggedSkills, NUM_TAGGED_SKILLS);
        traitsSetSelected((Trait)profile.selectedTraits[0], (Trait)profile.selectedTraits[1]);
    }
    if (changedSections == 0
        || (changedSections & (1u << (PROFILE_SECTION_PERKS - 1))) != 0) {
        perksSetRanks(profile.perkRanks, PERK_COUNT);
    }
    if (changedSections == 0
        || (changedSections & (1u << (PROFILE_SECTION_KILLS - 1))) != 0) {
        killsSetAll(profile.killCounts, KILL_TYPE_DEFAULT_COUNT);
    }
    if (changedSections == 0
        || (changedSections & (1u << (PROFILE_SECTION_SKILL_USE - 1))) != 0) {
        critterSetSneakWorking(profile.sneakWorking);
    }

    // Rebuild the local inventory graph from the canonical profile. The old
    // items are detached quietly (no sfx/hooks) and destroyed; the new graph
    // is rebuilt recursively with equipment flags restored, so hands/armor
    // resolve from the item flags exactly like the host's avatar.
    if (changedSections == 0
        || (changedSections & (1u << (PROFILE_SECTION_INVENTORY - 1))) != 0) {
        std::vector<std::pair<Object*, int>> oldItems;
        Inventory* inventory = &gDude->data.inventory;
        for (int index = 0; index < inventory->length; index++) {
            if (inventory->items[index].item != nullptr) {
                oldItems.emplace_back(inventory->items[index].item,
                    inventory->items[index].quantity);
            }
        }
        for (auto& entry : oldItems) {
            if (itemRemoveQuietly(gDude, entry.first, entry.second) != 0) {
                continue;
            }
            entry.first->flags &= ~OBJECT_NO_REMOVE;
            objectDestroy(entry.first, nullptr);
        }
        std::unordered_map<uint32_t, Object*> built;
        for (uint32_t root : profile.rootInventory) {
            if (!applyItemNode(profile, root, gDude, &built, 0)) {
                debugFilePrint("MPROF: apply local inventory rebuild failed root=%u", root);
                return false;
            }
        }
        // The inventory was rebuilt from the profile — the FID's weapon slot
        // never went through inven_wield, so re-derive it from the hands.
        MpSyncCritterWeaponFid(gDude);
    }
    debugFilePrint("MPROF: apply local done name='%s' items=%zu hp=%d",
        profile.name, profile.inventory.size(), profile.hp);
    return true;
}

MpPlayerRuntime* MpProfileGetRuntime(uint8_t netId)
{
    auto it = gRuntimes.find(netId);
    return it == gRuntimes.end() ? nullptr : &it->second;
}

MpPlayerRuntime* MpProfileFindRuntimeByObject(const Object* object)
{
    auto it = gObjectToRuntime.find(object);
    if (it == gObjectToRuntime.end()) return nullptr;
    return MpProfileGetRuntime(it->second);
}

// Destroy an object's inventory tree properly. The profile's items are
// created via objectCreateWithFidPid, which inserts them into the object
// head list (tile == -1). objectDestroy -> _obj_inven_free frees inventory
// items through a temporary node without unlinking the item's real list
// node, leaving dangling nodes that the next _obj_remove_all double-frees.
// Destroy children first (their slots are nulled as we go) so the critter
// teardown finds an empty inventory.
static void mpProfileDestroyObjectItems(Object* obj)
{
    if (obj == nullptr) {
        return;
    }
    Inventory* inv = &obj->data.inventory;
    for (int index = 0; index < inv->length; index++) {
        Object* item = inv->items[index].item;
        if (item == nullptr) {
            continue;
        }
        mpProfileDestroyObjectItems(item);
        item->flags &= ~OBJECT_NO_REMOVE;
        objectDestroy(item, nullptr);
        inv->items[index].item = nullptr;
    }
    // Items are gone; zero the length so a later _obj_inven_free (which
    // dereferences inventory->items[index].item without a null check)
    // skips the loop and only frees the items array once.
    inv->length = 0;
}

void MpProfileDetachAvatar(uint8_t netId)
{
    auto it = gRuntimes.find(netId);
    if (it == gRuntimes.end()) return;
    debugFilePrint("MPROF: detach avatar netId=%u name='%s'", netId, it->second.profile.name);
    if (it->second.object != nullptr) {
        gObjectToRuntime.erase(it->second.object);
        if (it->second.object != gDude) {
            mpProfileDestroyObjectItems(it->second.object);
            it->second.object->flags &= ~OBJECT_NO_REMOVE;
            objectDestroy(it->second.object, nullptr);
        }
    }
    it->second.object = nullptr;
}

void MpProfileDestroyRuntime(uint8_t netId)
{
    auto it = gRuntimes.find(netId);
    if (it == gRuntimes.end()) return;
    debugFilePrint("MPROF: destroy runtime netId=%u name='%s' pid=0x%X",
        netId, it->second.profile.name, it->second.syntheticPid);
    MpPlayerRuntime& runtime = it->second;
    if (runtime.object != nullptr) {
        gObjectToRuntime.erase(runtime.object);
        // Drop the reverse-map entry for the dying avatar so a stale
        // pointer can never linger on a destroyed object — the fresh
        // runtime re-registers on creation. The LOCAL dude is exempt: his
        // object is never destroyed here (the bind keeps it) and his
        // netId entry is the only thing that makes self-targeting
        // (MpGetObjNetId(gDude)) resolve.
        if (gMpIsClient && runtime.object != gDude && gMpSession.netIdToObj != nullptr) {
            for (int i = 1; i < gMpSession.netIdToObjCount; i++) {
                if (gMpSession.netIdToObj[i] == runtime.object) {
                    gMpSession.netIdToObj[i] = nullptr;
                }
            }
        }
        if (runtime.object != gDude) {
            mpProfileDestroyObjectItems(runtime.object);
            runtime.object->flags &= ~OBJECT_NO_REMOVE;
            objectDestroy(runtime.object, nullptr);
        }
    }
    if (runtime.syntheticPid != -1) protoRemove(runtime.syntheticPid);
    gRuntimes.erase(it);
}

void MpProfileDestroyAllRuntimes()
{
    debugFilePrint("MPROF: destroy all runtimes count=%zu", gRuntimes.size());
    while (!gRuntimes.empty()) MpProfileDestroyRuntime(gRuntimes.begin()->first);
    gObjectToRuntime.clear();
    artClearSessionModels();
    MpModelRegistryClear();
}

const char* MpProfileGetName(const Object* object)
{
    MpPlayerRuntime* runtime = MpProfileFindRuntimeByObject(object);
    return runtime != nullptr ? runtime->profile.name : nullptr;
}

bool MpProfileIsNetworkPlayer(const Object* object)
{
    return MpProfileFindRuntimeByObject(object) != nullptr;
}

int MpProfileGetPcStat(const Object* object, int pcStat)
{
    if (object == gDude) return pcGetStat((PcStat)pcStat);
    MpPlayerRuntime* runtime = MpProfileFindRuntimeByObject(object);
    if (runtime == nullptr || pcStat < 0 || pcStat >= PC_STAT_COUNT) return 0;
    return runtime->profile.pcStats[pcStat];
}

int MpProfileSetPcStat(Object* object, int pcStat, int value)
{
    if (object == gDude) return pcSetStat((PcStat)pcStat, value);
    MpPlayerRuntime* runtime = MpProfileFindRuntimeByObject(object);
    if (runtime == nullptr || pcStat < 0 || pcStat >= PC_STAT_COUNT) return -1;
    runtime->profile.pcStats[pcStat] = value;
    return 0;
}

bool MpProfileIsTagged(const Object* object, Skill skill)
{
    if (object == gDude) return skillIsTagged(skill);
    MpPlayerRuntime* runtime = MpProfileFindRuntimeByObject(object);
    if (runtime == nullptr) return false;
    for (int tagged : runtime->profile.taggedSkills) {
        if (tagged == skill) return true;
    }
    return false;
}

} // namespace fallout
