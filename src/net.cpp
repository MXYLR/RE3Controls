// Co-op: keep the partner's view of our character in step with what the mod does.
//
// RE5 replicates players by action changes plus 2 Hz heartbeats carrying position and yaw; the partner's game
// plays the action itself and blends toward the received values. The mod changes things the stock
// heartbeats can't express well:
//  - free camera: the character is steered by rotating it, so heartbeats are forced out whenever the facing
//    has changed noticeably (the partner's game blends them - works even if the partner has no mod);
//  - walk while aiming: the aim heartbeat carries no position, so a "wait" heartbeat (position + yaw) is sent
//    while walking, and when the partner runs the mod too, a small packet on our own P2P channel tells it which
//    leg clip to play on our character;
//  - aiming: the mod fires a pad player's shots at the crosshair (the camera ray), but the machine that owns an
//    enemy re-simulates the shooter's bullets along the barrel of its copy; the packet carries the aim point so
//    the copy fires there too;
//  - damage: the owning machine applies the multiplier, so the guest uses the host's values.
#include "crosshair.h"
#include "common.h"
#include "re5.h"
#include "config.h"
#include "net.h"
#include "aimwalk.h"
#include <math.h>
#include <string.h>

using namespace re5;

namespace {

constexpr uint32_t kAppPtr = 0x012340A4;            // +0x1042C -> session state, +0x56C online flag
constexpr uint32_t kBioNetworkPtr = 0x01249C1C;     // sBioNetwork
constexpr uint32_t kFnNetSend = 0x00792E70;         // thiscall(sBioNetwork, player, type, param) ret 0xC
constexpr uint32_t kSteamNetworkingIat = 0x00F4A464;  // steam_api!SteamNetworking
constexpr int kChannel = 0x5245;                    // the game uses channels 0 and 905
constexpr uint32_t kMagic = 0x34433352;             // 'R3C4' (v1.7: + fire rate, move speeds)

enum : uint8_t { PKT_HELLO = 1, PKT_STATE = 2 };
enum : uint8_t { F_AIMWALK = 1, F_AIM = 2, F_HOST = 4, F_BOB = 8 };  // F_BOB: hips follow the walk/run (v1.8)

#pragma pack(push, 1)
struct Packet {
    uint32_t magic;
    uint8_t type;
    uint8_t slot;    // character slot on both machines (character+0x2D9C)
    uint8_t flags;
    uint8_t blend;   // the leg layer's directional blend (aimwalk.cpp AimWalkLocalState; v1.7 sends 0)
    uint16_t clip;
    uint16_t spread;  // the sender's ShotgunSpread x 10000 + 1 (v1.8); 0 = not sent (v1.7: the original spread)
    float rate;
    float pos[3];
    float yaw;
    uint32_t seq;
    float twist;     // leg turn toward the real walking direction (diagonals)
    float aim[3];    // F_AIM: world point under the crosshair
    float damage;    // the sender's damage multipliers (the guest uses the host's)
    float knifeDamage;
    float fireRate;  // the sender's fire rate (the guest uses the host's)
    float walkMul;   // the sender's walk / run speed (its copy on the other machine moves the same)
    float runMul;
};
#pragma pack(pop)

// ISteamNetworking (SteamNetworking005) - only the slots the mod needs.
struct ISteamNetworking;
typedef bool(__thiscall* PFN_Send)(ISteamNetworking*, uint64_t id, const void* data, uint32_t len, int type, int ch);
typedef bool(__thiscall* PFN_Avail)(ISteamNetworking*, uint32_t* size, int ch);
typedef bool(__thiscall* PFN_Read)(ISteamNetworking*, void* dst, uint32_t cap, uint32_t* size, uint64_t* from, int ch);
typedef ISteamNetworking*(__cdecl* PFN_SteamNetworking)();
typedef void(__fastcall* PFN_NetSend)(uint32_t net, void* edx, uint32_t player, uint32_t type, uint32_t param);

struct State {
    double lastForce = 0, lastWaitBeat = 0, lastState = 0, lastHello = 0;
    float sentYaw = 0, sentPos[3] = {};
    bool wasAimWalking = false;
    bool partnerHasMod = false;
    uint32_t seq = 0;
    // from the partner
    uint32_t aimCh = 0;
    float aim[3] = {};
    double aimTime = 0;
    float hostDamage = 0, hostKnife = 1, hostFire = 1, hostSpread = 1;
    double hostTime = 0;
    float slotWalk[4] = {1, 1, 1, 1}, slotRun[4] = {1, 1, 1, 1};
    double slotTime[4] = {};
} N;

double Now() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

uint32_t Rd(uint32_t a) { return Readable((void*)a, 4) ? *(uint32_t*)a : 0; }

bool Online() {
    const uint32_t app = Rd(kAppPtr);
    const uint32_t st = app ? Rd(app + 0x1042C) : 0;
    return st && Readable((void*)(st + 0x56C), 1) && *(uint8_t*)(st + 0x56C) != 0 && Rd(kBioNetworkPtr) != 0;
}

// The host's index equals this machine's own (session +0x574 mine, +0x578 host).
bool IAmHost() {
    const uint32_t app = Rd(kAppPtr);
    const uint32_t st = app ? Rd(app + 0x1042C) : 0;
    return !st || !Readable((void*)(st + 0x574), 8) || At<uint32_t>(st, 0x574) == At<uint32_t>(st, 0x578);
}

ISteamNetworking* Steam() {
    const uint32_t fn = Rd(kSteamNetworkingIat);
    if (!fn) return nullptr;
    ISteamNetworking* s = ((PFN_SteamNetworking)fn)();
    return (s && Readable(s, 4) && Readable(*(void**)s, 12)) ? s : nullptr;
}

// Peer SteamIDs of the game's own P2P layer (NetworkMultipleUdpSteam +0x18, 16 slots).
int Peers(uint64_t* out, int max) {
    const uint32_t net = Rd(kBioNetworkPtr);
    const uint32_t mgr = net ? Rd(net + 0x38) : 0;
    const uint32_t udp = mgr ? Rd(mgr + 0xAD04) : 0;
    if (!udp || !Readable((void*)(udp + 0x18), 16 * 8)) return 0;
    int n = 0;
    for (int i = 0; i < 16 && n < max; ++i) {
        const uint64_t id = *(uint64_t*)(udp + 0x18 + i * 8);
        if (id) out[n++] = id;
    }
    return n;
}

void SendAll(ISteamNetworking* s, const Packet& p) {
    uint64_t peers[16];
    const int n = Peers(peers, 16);
    auto send = (PFN_Send)(*(void***)s)[0];
    for (int i = 0; i < n; ++i) send(s, peers[i], &p, sizeof(p), 0 /* unreliable */, kChannel);
}

void Receive(ISteamNetworking* s) {
    auto avail = (PFN_Avail)(*(void***)s)[1];
    auto read = (PFN_Read)(*(void***)s)[2];
    uint32_t size = 0;
    for (int guard = 0; guard < 32 && avail(s, &size, kChannel); ++guard) {
        Packet p;
        uint64_t from = 0;
        uint32_t got = 0;
        if (!read(s, &p, sizeof(p), &got, &from, kChannel)) break;
        if (got != sizeof(p) || p.magic != kMagic) continue;
        N.partnerHasMod = true;
        if (p.flags & F_HOST) {
            N.hostDamage = p.damage;
            N.hostKnife = p.knifeDamage;
            N.hostFire = p.fireRate;
            N.hostSpread = p.spread && p.spread <= 10001 ? (float)(p.spread - 1) / 10000.0f : 1.0f;
            N.hostTime = Now();
        }
        if (p.slot <= 3) {
            N.slotWalk[p.slot] = p.walkMul;
            N.slotRun[p.slot] = p.runMul;
            N.slotTime[p.slot] = Now();
        }
        if (p.type != PKT_STATE || p.slot > 3) continue;
        const uint32_t mgr = Rd(kPlayerMgrPtr);
        const uint32_t ch = mgr ? Rd(mgr + 0x24 + p.slot * 4) : 0;
        if (!ch || !Readable((void*)ch, 0x3000) || At<uint8_t>(ch, ch::NetControl) != 2) continue;
        AimWalkRemote(ch, (p.flags & F_AIMWALK) != 0, p.clip, p.rate, p.twist, p.pos, p.yaw, p.blend,
                      (p.flags & F_BOB) != 0);
        if (p.flags & F_AIM) {
            N.aimCh = ch;
            memcpy(N.aim, p.aim, 12);
            N.aimTime = Now();
        } else if (N.aimCh == ch) {
            N.aimTime = 0;
        }
    }
}

}  // namespace

void NetLocalFrame(uint32_t ch, bool inLocomotion, bool aimWalking) {
    if (!Cfg().coopSync || !Online() || At<uint8_t>(ch, ch::NetControl) != 0) return;
    const double now = Now();
    const float* pos = (const float*)(ch + ch::Pos);
    const float* q = (const float*)(ch + ch::Quat);
    const float yaw = 2.0f * atan2f(q[1], q[3]);
    float dyaw = yaw - N.sentYaw;
    while (dyaw > 3.14159265f) dyaw -= 6.2831853f;
    while (dyaw < -3.14159265f) dyaw += 6.2831853f;
    const float dx = pos[0] - N.sentPos[0], dz = pos[2] - N.sentPos[2];

    // 1. Free camera steering: let the game send its locomotion heartbeat now instead of in 0.5 s.
    if (inLocomotion && (fabsf(dyaw) > 0.05f || dx * dx + dz * dz > 40.0f * 40.0f) && now - N.lastForce > 0.066) {
        At<float>(ch, ch::NetTimer0) = 0.0f;
        At<float>(ch, ch::NetTimer1) = 0.0f;
        N.lastForce = now;
        N.sentYaw = yaw;
        memcpy(N.sentPos, pos, 12);
    }

    // 2. Walking while aiming: the aim heartbeat has no position, send the "wait" one (position + yaw).
    const uint32_t net = Rd(kBioNetworkPtr);
    if ((aimWalking || N.wasAimWalking) && net && now - N.lastWaitBeat > 0.08) {
        ((PFN_NetSend)kFnNetSend)(net, nullptr, ch, 4, 0);
        N.lastWaitBeat = now;
        N.sentYaw = yaw;
        memcpy(N.sentPos, pos, 12);
    }

    // 3. Mod-to-mod channel: leg clip for the partner's copy of us.
    ISteamNetworking* s = Steam();
    if (s) {
        Receive(s);
        Packet p = {};
        p.magic = kMagic;
        p.slot = At<uint8_t>(ch, ch::NetSlot);
        p.damage = Cfg().damage;
        p.knifeDamage = Cfg().knifeDamage;
        p.fireRate = Cfg().fireRate;
        p.spread = (uint16_t)(Cfg().shotgunSpread * 10000.0f + 0.5f) + 1;
        p.walkMul = Cfg().moveWalk;
        p.runMul = Cfg().moveRun;
        if (IAmHost()) p.flags |= F_HOST;
        float aim[3];
        const bool aiming = LocalAimPoint(ch, aim);
        if (!N.partnerHasMod) {
            if (now - N.lastHello > 2.0) {  // tiny, rare: an unmodded partner just ignores this channel
                p.type = PKT_HELLO;
                SendAll(s, p);
                N.lastHello = now;
            }
        } else {
            uint16_t clip = 0xFFFF;
            float rate = 1.0f, twist = 0.0f;
            uint8_t blend = 0;
            bool bob = false;
            const bool walking = aimWalking && AimWalkLocalState(ch, &clip, &rate, &twist, &blend, &bob);
            const double period = walking || aiming ? 0.066 : 0.5;
            if (walking != N.wasAimWalking || now - N.lastState > period) {
                p.type = PKT_STATE;
                p.flags |= walking ? F_AIMWALK : 0;
                p.flags |= walking && bob ? F_BOB : 0;
                if (aiming) {
                    p.flags |= F_AIM;
                    memcpy(p.aim, aim, 12);
                }
                p.clip = clip;
                p.rate = rate;
                p.twist = twist;
                p.blend = blend;
                memcpy(p.pos, pos, 12);
                p.yaw = yaw;
                p.seq = ++N.seq;
                SendAll(s, p);
                N.lastState = now;
            }
        }
    }
    N.wasAimWalking = aimWalking;
    AimWalkRemoteTick();
}

bool NetRemoteAim(uint32_t ch, float* aimPoint) {
    if (!Cfg().coopSync || ch != N.aimCh || Now() - N.aimTime > 0.3) return false;
    memcpy(aimPoint, N.aim, 12);
    return true;
}

float NetFireRate() {
    // A guest fires at the host's rate, or the original rate while there is none (an unmodded host, Sync=0):
    // the host replays the guest's shots at its own pace.
    if (Online() && !IAmHost())
        return Cfg().coopSync && N.hostTime > 0 && Now() - N.hostTime < 5.0 ? N.hostFire : 1.0f;
    return Cfg().fireRate;
}

float NetShotgunSpread() {
    // The machine that owns an enemy simulates every player's pellets itself, so both machines use the host's value
    // (the original spread while there is none: an unmodded or v1.7 host, Sync=0).
    if (Online() && !IAmHost())
        return Cfg().coopSync && N.hostTime > 0 && Now() - N.hostTime < 5.0 ? N.hostSpread : 1.0f;
    return Cfg().shotgunSpread;
}

void NetMoveSpeeds(uint32_t ch, float* walk, float* run) {
    *walk = Cfg().moveWalk;
    *run = Cfg().moveRun;
    if (Readable((void*)ch, 0x3000) && At<uint8_t>(ch, ch::NetControl) == 2) {
        // The partner's copy moves at the partner's speed (their packets), or the original speed without them.
        const uint8_t slot = At<uint8_t>(ch, ch::NetSlot);
        const bool fresh = slot <= 3 && N.slotTime[slot] > 0 && Now() - N.slotTime[slot] < 5.0;
        *walk = fresh ? N.slotWalk[slot] : 1.0f;
        *run = fresh ? N.slotRun[slot] : 1.0f;
    }
}

void NetDamageValues(float* mult, float* knife) {
    *mult = Cfg().damage;
    *knife = Cfg().knifeDamage;
    if (Cfg().coopSync && N.hostTime > 0 && Now() - N.hostTime < 5.0 && Online() && !IAmHost()) {
        *mult = N.hostDamage;
        *knife = N.hostKnife;
    }
}
