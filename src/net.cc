#include "net.h"

// Windows sockets first — ENet pulls winsock2 in on its own, but the raw
// discovery socket below needs the types declared before anything else.
#ifdef _WIN32
#include <winsock2.h>
#endif
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
// LAN discovery (raw UDP on NET_LAN_DISCOVERY_PORT, separate from the ENet
// session socket). The host answers probes with its session facts; the
// browser broadcasts a probe and collects replies.
// ---------------------------------------------------------------------------

// NOTE: the handles are stored as int on BOTH platforms. Windows SOCKET is
// an unsigned 64-bit type, so comparing it with < 0 / >= 0 is always false
// and INVALID_SOCKET would pass every "valid" check — the browser socket was
// never created. Windows socket handles fit in 32 bits and INVALID_SOCKET
// truncates to -1, so int semantics work everywhere.
static int gNetLanListener = -1;
static int gNetLanBrowserSocket = -1;

// The advertised session template (filled by NetLanSetReplyInfo; the player
// count is refreshed by multiplayer.cc via the same call).
static NetLanReply gNetLanReplyTemplate = {};

static bool netLanSocketSetNonBlocking(int socketHandle)
{
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket((SOCKET)socketHandle, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(socketHandle, F_GETFL, 0);
    return flags != -1 && fcntl(socketHandle, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

// Bind a UDP socket to INADDR_ANY:port in non-blocking mode. Returns the
// socket handle or -1.
static int netLanCreateBoundSocket(uint16_t port)
{
#ifdef _WIN32
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        return -1;
    }
#else
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        return -1;
    }
#endif
    int enable = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&enable, sizeof(enable));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(s, (const sockaddr*)&addr, sizeof(addr)) != 0) {
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
        return -1;
    }
    if (!netLanSocketSetNonBlocking(s)) {
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
        return -1;
    }
    return s;
}

static void netLanCloseSocket(int socketHandle)
{
    if (socketHandle < 0) {
        return;
    }
#ifdef _WIN32
    closesocket((SOCKET)socketHandle);
#else
    close(socketHandle);
#endif
}

void NetLanSetReplyInfo(const char* hostName, uint16_t hostPort,
    uint16_t maxPlayers, uint16_t currentPlayers, bool passwordRequired)
{
    netLanCloseSocket(gNetLanListener);
    gNetLanListener = -1;

    if (hostName == nullptr || hostName[0] == '\0') {
        MpLog(MP_LOG_NET, "lan reply listener closed");
        return;
    }

    gNetLanReplyTemplate.magic = NET_LAN_MAGIC;
    gNetLanReplyTemplate.versionHash = NetGetVersionHash();
    strncpy(gNetLanReplyTemplate.hostName, hostName,
        sizeof(gNetLanReplyTemplate.hostName) - 1);
    gNetLanReplyTemplate.hostName[sizeof(gNetLanReplyTemplate.hostName) - 1] = '\0';
    gNetLanReplyTemplate.hostPort = hostPort;
    gNetLanReplyTemplate.maxPlayers = maxPlayers;
    gNetLanReplyTemplate.currentPlayers = currentPlayers;
    gNetLanReplyTemplate.passwordRequired = passwordRequired ? 1 : 0;

    gNetLanListener = netLanCreateBoundSocket(NET_LAN_DISCOVERY_PORT);
    if (gNetLanListener < 0) {
        MpLogAlways(MP_LOG_NET, "lan reply listener bind failed port=%u", NET_LAN_DISCOVERY_PORT);
        return;
    }
    MpLog(MP_LOG_NET, "lan reply listener open name='%s' port=%u players=%u/%u pass=%d",
        hostName, hostPort, currentPlayers, maxPlayers, passwordRequired ? 1 : 0);
}

// Answer any probe with a valid magic on the discovery port (the browser
// judges version compatibility itself so a mismatched build can be flagged).
static void netLanListenerDrain()
{
    if (gNetLanListener < 0) {
        return;
    }
    NetLanProbe probe;
    sockaddr_in from = {};
    int fromLen = sizeof(from);
    for (;;) {
        int received = recvfrom(gNetLanListener, (char*)&probe, sizeof(probe), 0,
            (sockaddr*)&from, &fromLen);
        if (received < 0) {
            break; // EWOULDBLOCK / EAGAIN — nothing pending
        }
        if (received != (int)sizeof(probe) || probe.magic != NET_LAN_MAGIC) {
            continue;
        }
        NetLanReply reply = gNetLanReplyTemplate;
        reply.magic = NET_LAN_MAGIC;
        reply.versionHash = NetGetVersionHash();
        reply.hostPort = gNetBoundPort != 0 ? gNetBoundPort : reply.hostPort;
        if (sendto(gNetLanListener, (const char*)&reply, sizeof(reply), 0,
                (const sockaddr*)&from, sizeof(from)) < 0) {
            // Transient error (e.g. ICMP port unreachable from a browser that
            // closed) — keep listening.
            break;
        }
    }
}

bool NetLanBrowserStart()
{
    if (gNetLanBrowserSocket >= 0) {
        return true;
    }
    gNetLanBrowserSocket = netLanCreateBoundSocket(0); // ephemeral local port
    if (gNetLanBrowserSocket < 0) {
        MpLogAlways(MP_LOG_NET, "lan browser socket open failed");
        return false;
    }
    int enable = 1;
    setsockopt(gNetLanBrowserSocket, SOL_SOCKET, SO_BROADCAST,
        (const char*)&enable, sizeof(enable));
    MpLog(MP_LOG_NET, "lan browser socket open");
    return true;
}

void NetLanBrowserStop()
{
    netLanCloseSocket(gNetLanBrowserSocket);
    gNetLanBrowserSocket = -1;
    MpLog(MP_LOG_NET, "lan browser socket closed");
}

void NetLanBrowserScan()
{
    if (gNetLanBrowserSocket < 0) {
        return;
    }
    NetLanProbe probe = {};
    probe.magic = NET_LAN_MAGIC;
    probe.versionHash = NetGetVersionHash();

    sockaddr_in target = {};
    target.sin_family = AF_INET;
    target.sin_port = htons(NET_LAN_DISCOVERY_PORT);

    // Broadcast covers the LAN; the loopback probe finds a host running on
    // this same machine (common for local co-op testing).
    target.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(gNetLanBrowserSocket, (const char*)&probe, sizeof(probe), 0,
        (const sockaddr*)&target, sizeof(target));
    target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(gNetLanBrowserSocket, (const char*)&probe, sizeof(probe), 0,
        (const sockaddr*)&target, sizeof(target));
}

int NetLanBrowserPoll(NetLanHostInfo* hosts, int capacity)
{
    if (gNetLanBrowserSocket < 0 || hosts == nullptr || capacity <= 0) {
        return 0;
    }
    uint32_t ownVersion = NetGetVersionHash();
    NetLanReply reply;
    sockaddr_in from = {};
    int fromLen = sizeof(from);
    int count = 0;
    for (;;) {
        int received = recvfrom(gNetLanBrowserSocket, (char*)&reply, sizeof(reply), 0,
            (sockaddr*)&from, &fromLen);
        if (received < 0) {
            break;
        }
        if (received != (int)sizeof(reply) || reply.magic != NET_LAN_MAGIC) {
            continue;
        }
        // Deduplicate by address+port (repeated probes/answers).
        bool duplicate = false;
        for (int i = 0; i < count; i++) {
            if (hosts[i].port == reply.hostPort
                && strcmp(hosts[i].address, inet_ntoa(from.sin_addr)) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        if (count >= capacity) {
            break;
        }
        NetLanHostInfo* host = &hosts[count];
        strncpy(host->address, inet_ntoa(from.sin_addr), sizeof(host->address) - 1);
        host->address[sizeof(host->address) - 1] = '\0';
        host->port = reply.hostPort;
        strncpy(host->name, reply.hostName, sizeof(host->name) - 1);
        host->name[sizeof(host->name) - 1] = '\0';
        host->maxPlayers = reply.maxPlayers;
        host->currentPlayers = reply.currentPlayers;
        host->passwordRequired = reply.passwordRequired != 0;
        host->versionHash = reply.versionHash;
        count++;
        MpLog(MP_LOG_NET, "lan host found '%s' %s:%u players=%u/%u pass=%d version=%s",
            host->name, host->address, host->port,
            host->currentPlayers, host->maxPlayers,
            host->passwordRequired ? 1 : 0,
            host->versionHash == ownVersion ? "match" : "MISMATCH");
    }
    return count;
}

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

    // Co-op LAN discovery: answer any pending probe on the fixed discovery
    // port. Runs on the host's existing service cadence (non-blocking).
    netLanListenerDrain();

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
    // The hash is constant per build; log it once so it stops spamming every
    // LAN poll frame.
    static uint32_t sLastLoggedVersionHash = 0;
    if (hash != sLastLoggedVersionHash) {
        sLastLoggedVersionHash = hash;
        MpLog(MP_LOG_NET, "version hash=%08X src='%s'", hash, buf);
    }
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
