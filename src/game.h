#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// Elden Ring memory: singletons, characters and the camera.
//
// Singletons are found by name, without hard-coded addresses: every access to a FromSoftware
// singleton is preceded by the same null check ("mov reg,[slot]; test; jnz; lea rcx,[runtime
// class]; call ...; FD4Singleton.h assert"). The runtime class object and the instance carry MSVC
// RTTI, which gives the class name (e.g. "WorldChrManImp", "CSCameraImp"). This keeps working after
// game patches as long as the classes keep their names.
namespace zz::game {

namespace off {
// WorldChrManImp (checked in game 1.17.1: ChrSets are embedded, found by RTTI at run time)
constexpr ptrdiff_t kWcmSummonBuddyChrSet = 0x10F90;  // inline ChrSet: spirit ashes and Torrent
constexpr ptrdiff_t kWcmPlayerIns = 0x1E508;
// ChrSet
constexpr ptrdiff_t kChrSetCapacity = 0x10;  // u32
constexpr ptrdiff_t kChrSetEntries = 0x18;   // {ChrIns*, ...}[capacity], 0x10 bytes each
// ChrIns
constexpr ptrdiff_t kCharacterId = 0x188;
constexpr ptrdiff_t kNpcParamId = 0x60;  // NpcParam row, 0 / -1 for real players
constexpr ptrdiff_t kNpcId = 0x64;       // 4-digit character id (8000 = Torrent)
constexpr ptrdiff_t kChrType = 0x68;     // i32: 0 local, 1 white phantom, 10 bloodstain ghost...
constexpr ptrdiff_t kModules = 0x190;
constexpr ptrdiff_t kSpecialEffect = 0x178;  // CSSpecialEffect*
// CSSpecialEffect: list of active SpEffects (checked in 1.17.1, see buffs.cpp)
constexpr ptrdiff_t kEffectHead = 0x8;
constexpr ptrdiff_t kEffectNext = 0x30;
constexpr ptrdiff_t kEffectTimeLeft = 0x44;   // float seconds
constexpr ptrdiff_t kEffectParamRow = 0x0;    // -> SpEffectParam row
// SpEffectParam row
constexpr ptrdiff_t kParamDuration = 0x8;     // float seconds, -1 = permanent
constexpr ptrdiff_t kParamStateInfo = 0x156;  // u16: which status the effect is
// Module container
constexpr ptrdiff_t kModData = 0x00;
constexpr ptrdiff_t kModResist = 0x20;
constexpr ptrdiff_t kModSuperArmor = 0x40;
constexpr ptrdiff_t kModPhysics = 0x68;
// CSChrDataModule
constexpr ptrdiff_t kDataHp = 0x138;
constexpr ptrdiff_t kDataMaxHp = 0x13C;
// CSChrResistModule: per status a gauge that starts at the resistance and falls with every hit
// (build-up = resistance - gauge); it procs below 1 and refills to the resistance.
constexpr ptrdiff_t kResistGauge = 0x10;       // i32[7]
constexpr ptrdiff_t kResistResistance = 0x2C;  // i32[7], 999 = immune
// CSChrSuperArmorModule (poise / stance)
constexpr ptrdiff_t kSuperArmorCurrent = 0x10;
constexpr ptrdiff_t kSuperArmorMax = 0x14;
// CSChrPhysicsModule
constexpr ptrdiff_t kPhysicsPosition = 0x70;
}  // namespace off

// Scans the executable for singleton accesses. Call once (takes ~0.5 s) after the game window
// exists; names are resolved lazily afterwards because instances are created later.
void Init();
bool Initialized();
// Slot (static address holding the instance pointer) of a singleton class, 0 if unknown.
uintptr_t SingletonSlot(const char* name);
// The live instance, 0 if unknown or not created yet.
uintptr_t Singleton(const char* name);
std::string SingletonReport();

uintptr_t WorldChrMan();
uintptr_t PlayerIns();
uintptr_t ChrModule(uintptr_t chr, ptrdiff_t module_offset);
bool InGame();
bool Riding();  // on Torrent
// Number of players in the world (you included); > 1 in co-op / invasions.
int PlayerCount();

// Status ailments in CSChrResistModule order.
enum Status { kPoison, kScarletRot, kBloodLoss, kDeathBlight, kFrostbite, kSleep, kMadness, kStatusCount };

struct Chr {
    uintptr_t ins = 0;
    int hp = 0, max_hp = 0;
    float sa = 0, sa_max = 0;  // super armor (stance / poise) left and its maximum
    float pos[3] = {};         // physics position (map-local)
    bool player = false;       // a player (you or another one)
    bool buddy = false;        // spirit ash / Torrent
    // Status build-up so far and the amount that procs it; resistance 0 = unknown / immune.
    int buildup[kStatusCount] = {};
    int resistance[kStatusCount] = {};
};
bool ReadChr(uintptr_t ins, Chr& out);

// Statuses that are in effect on a character (poisoned, rotting, frostbitten, asleep...): bit i =
// Status i, with the seconds left / total of each (0 when the effect has no timer).
struct ActiveStatus {
    uint32_t mask = 0;
    float left[kStatusCount] = {};
    float total[kStatusCount] = {};
};
ActiveStatus ReadActiveStatus(uintptr_t ins);
// Still a live character (its vtable is a known ChrIns class)?
bool IsChr(uintptr_t ins);
// Every loaded character (players included); with `center`, only those within `radius` metres.
void ForEachChr(const std::function<void(const Chr&)>& fn, const float* center = nullptr,
                float radius = 0);

// Camera used for rendering: world matrix rows (right, up, forward, position), vertical FOV in
// radians and aspect ratio.
struct Camera {
    float right[3], up[3], forward[3], pos[3];
    float fov_y = 0, aspect = 0;
};
bool ReadCamera(Camera& out);
// Projects a world (map-local) point to screen pixels; false if behind the camera.
bool WorldToScreen(const Camera& cam, const float p[3], float width, float height, float out[2]);

}  // namespace zz::game
