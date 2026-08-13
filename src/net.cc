#include "net.h"

#include <enet/enet.h>
#include <string.h>
#include <stdio.h>

#include "debug.h"
#include "platform/git_version.h"
#include "version.h"
#include "multiplayer_log.h"

namespace fallout {

// Last-bound host port (NetHostCreate) and last-connect client endpoint
// (NetClientConnect), so the UI can show "Hosting on port X" / "Joined
// a.b.c.d:port" without touching ENet types.
static uint16_t gNetBoundPort = NET_DEFAULT_PORT;
static char gNetConnectedAddress[64] = "";
static uint16_t gNetConnectedPort = 0;

// ---------------------------------------------------------------------------
// Global init / shutdown
// ---------------------------------------------------------------------------

bool NetInit()
{
    return enet_initialize() == 0;
}

void NetShutdown()
{
    enet_deinitialize();
}

// ---------------------------------------------------------------------------
// Host / client creation
// ---------------------------------------------------------------------------

// The join burst is ~8MB of profile chunks over a default 64KB UDP buffer;
// the receiver overflows and relies on ENet retransmits, which stall the
// whole transfer if the sender's main loop freezes. Grow both socket buffers
// so the burst fits end-to-end (loopback typically honors these sizes).
static void netGrowSocketBuffers(ENetHost* host)
{
    if (host == nullptr || host->socket == ENET_SOCKET_NULL) {
        return;
    }
    int rcvbuf = 8 * 1024 * 1024;
    int sndbuf = 8 * 1024 * 1024;
    setsockopt(host->socket, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvbuf, sizeof(rcvbuf));
    setsockopt(host->socket, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf));
}

ENetHost* NetHostCreate(uint16_t port, int maxPeers)
{
    ENetAddress addr;
    addr.host = ENET_HOST_ANY;
    addr.port = port;
    ENetHost* host = enet_host_create(&addr, maxPeers, NET_NUM_CHANNELS, 0, 0);
    netGrowSocketBuffers(host);
    gNetBoundPort = port;
    MpLog(MP_LOG_NET, "host create port=%u peers=%d result=%p", port, maxPeers, (void*)host);
    return host;
}

ENetHost* NetClientCreate()
{
    ENetHost* host = enet_host_create(nullptr, 1, NET_NUM_CHANNELS, 0, 0);
    netGrowSocketBuffers(host);
    MpLog(MP_LOG_NET, "client create result=%p", (void*)host);
    return host;
}

ENetPeer* NetClientConnect(ENetHost* client, const char* address, uint16_t port)
{
    if (client == nullptr || address == nullptr) {
        MpLogAlways(MP_LOG_NET, "client connect failed null client/address");
        return nullptr;
    }
    ENetAddress addr;
    if (enet_address_set_host(&addr, address) != 0) {
        MpLogAlways(MP_LOG_NET, "client connect address resolve failed '%s'", address);
        return nullptr;
    }
    addr.port = port;
    ENetPeer* peer = enet_host_connect(client, &addr, NET_NUM_CHANNELS, 0);
    strncpy(gNetConnectedAddress, address, sizeof(gNetConnectedAddress) - 1);
    gNetConnectedAddress[sizeof(gNetConnectedAddress) - 1] = '\0';
    gNetConnectedPort = port;
    MpLog(MP_LOG_NET, "client connect '%s:%u' result=%p", address, port, (void*)peer);
    return peer;
}

void NetPeerDisconnect(ENetPeer* peer)
{
    if (peer != nullptr) {
        enet_peer_disconnect(peer, 0);
    }
}

void NetHostDestroy(ENetHost* host)
{
    if (host != nullptr) {
        enet_host_destroy(host);
    }
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

static enet_uint32 channelFlags(int channel)
{
    if (channel == NET_CHANNEL_RELIABLE) {
        return ENET_PACKET_FLAG_RELIABLE;
    }
    return 0;
}

bool NetPeerSend(ENetPeer* peer, int channel, const void* data, size_t dataLength)
{
    if (peer == nullptr || data == nullptr || dataLength == 0) {
        return false;
    }
    ENetPacket* packet = enet_packet_create(data, dataLength, channelFlags(channel));
    if (packet == nullptr) {
        return false;
    }
    return enet_peer_send(peer, (enet_uint8)channel, packet) == 0;
}

bool NetHostBroadcast(ENetHost* host, int channel, const void* data, size_t dataLength)
{
    if (host == nullptr || data == nullptr || dataLength == 0) {
        return false;
    }
    ENetPacket* packet = enet_packet_create(data, dataLength, channelFlags(channel));
    if (packet == nullptr) {
        return false;
    }
    enet_host_broadcast(host, (enet_uint8)channel, packet);
    return true;
}

// ---------------------------------------------------------------------------
// Host service / event dispatch
//
//eventType: 1 = connect, 2 = disconnect, 3 = receive.
// For receive, the caller (callback) is handed a pointer to the raw packet
// buffer (header + payload) and its length. The callback does NOT own this
// buffer — it is destroyed right after the callback returns.
// ---------------------------------------------------------------------------

void NetHostService(ENetHost* host, NetEventCallback callback, void* userData)
{
    if (host == nullptr || callback == nullptr) {
        return;
    }

    ENetEvent event;
    while (enet_host_service(host, &event, 0) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT:
            callback(event.peer, 1, nullptr, 0, userData);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            callback(event.peer, 2, nullptr, 0, userData);
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            callback(event.peer, 3, event.packet->data, event.packet->dataLength, userData);
            enet_packet_destroy(event.packet);
            break;
        default:
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Version hash (FNV-1a 32-bit)
// ---------------------------------------------------------------------------

uint32_t NetGetVersionHash()
{
    char versionBuf[64];
    versionGetVersion(versionBuf, sizeof(versionBuf));

    // Append the git build hash if available, so two builds of the same
    // engine version from different source revisions don't negotiate
    // against each other via the same version string.
    char buf[128];
    snprintf(buf, sizeof(buf), "%s|%s", versionBuf, _BUILD_HASH);

    // FNV-1a 32-bit
    uint32_t hash = 0x811c9dc5u;
    for (const char* p = buf; *p != '\0'; p++) {
        hash ^= (uint32_t)(unsigned char)(*p);
        hash *= 0x01000193u;
    }
    MpLog(MP_LOG_NET, "version hash=%08X src='%s'", hash, buf);
    return hash;
}

// FNV-1a 32-bit hash of a session password. 0 for empty input — the "no
// password" sentinel. The password itself is never stored, sent or logged in
// plaintext; only this hash rides the handshake.
uint32_t NetPasswordHash(const char* password)
{
    if (password == nullptr || password[0] == '\0') {
        return 0;
    }
    uint32_t hash = 0x811c9dc5u;
    for (const char* p = password; *p != '\0'; p++) {
        hash ^= (uint32_t)(unsigned char)(*p);
        hash *= 0x01000193u;
    }
    return hash;
}

uint16_t NetGetBoundPort()
{
    return gNetBoundPort;
}

const char* NetGetConnectedAddress()
{
    return gNetConnectedAddress;
}

uint16_t NetGetConnectedPort()
{
    return gNetConnectedPort;
}

uint32_t NetGetPingMs(ENetHost* host, ENetPeer* hostPeer)
{
    if (hostPeer != nullptr) {
        // Client: ping to the host.
        return hostPeer->roundTripTime;
    }
    if (host == nullptr) {
        return 0;
    }
    // Host: average ping of the connected peers.
    uint64_t total = 0;
    int count = 0;
    for (size_t i = 0; i < host->peerCount; i++) {
        ENetPeer* peer = &host->peers[i];
        if (peer->state == ENET_PEER_STATE_CONNECTED) {
            total += peer->roundTripTime;
            count++;
        }
    }
    return count > 0 ? (uint32_t)(total / count) : 0;
}

// ---------------------------------------------------------------------------
// Convenience: build a packet (header + payload) and send it.
// ---------------------------------------------------------------------------

static bool buildPacket(uint8_t packetType, const void* payload, size_t payloadLength,
    uint8_t* outBuffer, size_t outBufferCapacity, size_t* outDataLength)
{
    if (payloadLength > 0xFFFF) {
        return false; // NetPacketHeader.length is uint16
    }
    size_t total = sizeof(NetPacketHeader) + payloadLength;
    if (total > outBufferCapacity) {
        return false;
    }
    NetPacketHeader* hdr = reinterpret_cast<NetPacketHeader*>(outBuffer);
    hdr->type = packetType;
    hdr->length = (uint16_t)payloadLength;
    if (payload != nullptr && payloadLength > 0) {
        memcpy(outBuffer + sizeof(NetPacketHeader), payload, payloadLength);
    }
    *outDataLength = total;
    return true;
}

bool NetSendPacket(ENetPeer* peer, int channel, uint8_t packetType,
    const void* payload, size_t payloadLength)
{
    if (peer == nullptr) {
        return false;
    }
    uint8_t buf[NET_MAX_PACKET_SIZE];
    size_t dataLength = 0;
    if (!buildPacket(packetType, payload, payloadLength, buf, sizeof(buf), &dataLength)) {
        return false;
    }
    return NetPeerSend(peer, channel, buf, dataLength);
}

bool NetBroadcastPacket(ENetHost* host, int channel, uint8_t packetType,
    const void* payload, size_t payloadLength)
{
    if (host == nullptr) {
        return false;
    }
    uint8_t buf[NET_MAX_PACKET_SIZE];
    size_t dataLength = 0;
    if (!buildPacket(packetType, payload, payloadLength, buf, sizeof(buf), &dataLength)) {
        return false;
    }
    return NetHostBroadcast(host, channel, buf, dataLength);
}

} // namespace fallout
