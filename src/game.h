#pragma once
#include "instance.h"
#include "math.h"
#include <string>
#include <vector>
#include <cstdint>

namespace game {

struct Vec2 { float x{}, y{}; };

// Roblox's client area in screen coordinates. The overlay covers the whole
// monitor, so ESP/aim math must project into this rect, not the overlay rect
// (otherwise everything shifts as soon as the game is windowed).
struct Viewport {
    float x = 0.0f, y = 0.0f;
    float w = 1.0f, h = 1.0f;
};
Viewport robloxViewport();

struct Camera {
    Vec3 right{1, 0, 0};
    Vec3 up{0, 1, 0};
    Vec3 look{0, 0, -1};
    Vec3 pos{};
    float fov = 70.0f;
};

// Joints we can resolve from the character's parts. R15 characters fill the
// named limbs; R6 characters fill Torso/Left Arm/Right Arm/Left Leg/Right Leg.
enum Bone : int {
    BoneHead = 0,
    BoneUpperTorso,
    BoneLowerTorso,
    BoneLeftUpperArm, BoneLeftLowerArm, BoneLeftHand,
    BoneRightUpperArm, BoneRightLowerArm, BoneRightHand,
    BoneLeftUpperLeg, BoneLeftLowerLeg, BoneLeftFoot,
    BoneRightUpperLeg, BoneRightLowerLeg, BoneRightFoot,
    BoneTorso,                                  // R6
    BoneLeftArm, BoneRightArm,                  // R6
    BoneLeftLeg, BoneRightLeg,                  // R6
    BoneCount
};

enum class Kind : uint8_t { Player = 0, Mob = 1 };

struct Entity {
    Kind kind = Kind::Player;
    uintptr_t playerAddr = 0;
    uintptr_t characterAddr = 0;
    uintptr_t humanoidAddr = 0;
    uintptr_t headAddr = 0;
    uintptr_t rootAddr = 0;
    uintptr_t torsoAddr = 0;
    uintptr_t teamAddr = 0;
    std::string name;
    std::string label;      // "player", or the mob classification
    std::string toolName;   // equipped Tool, when enabled
    float health = 100.0f;
    float maxHealth = 100.0f;
    Vec3 headPos{};
    Vec3 rootPos{};
    Vec3 torsoPos{};
    Vec3 boxTop{};      // top of the head, in world space
    Vec3 boxBottom{};   // feet, in world space
    Vec3 boxHalf{};     // half extents used by the 3D box (x = width, z = depth)
    Vec3 bones[BoneCount]{};
    uint32_t boneMask = 0;   // bit i set = bones[i] is valid
    bool r15 = false;
    bool ghost = false;      // streamed out: position is no longer being updated
    float distance = 0.0f;
    std::string teamSig;     // kit/uniform signature, used when the game has no Roblox Team

    // Stable identity for sticky targeting and prediction. Mobs have no Player
    // object, so the character model is the key.
    uintptr_t key() const { return playerAddr ? playerAddr : characterAddr; }
    float height() const { return boxTop.y - boxBottom.y; }
};

// Sorted, cached signature of the character's kit parts (armour model, uniform
// accessories). Two players on the same side wear the same kit, so it is a
// usable team key in games that never create Roblox Team objects.
std::string teamSignature(uintptr_t character);

// Returns false while Roblox is not attached / no DataModel.
bool readCamera(Camera& out);
Vec3 readPartPosition(uintptr_t part);
uintptr_t readPrimitive(uintptr_t part);

// Enumerates every Player with a live character. Local player excluded.
// playersSeen (optional) receives the Player count, which lets callers tell
// "nobody loaded yet" apart from "we left the game".
std::vector<Entity> collectEntities(const Camera& cam, bool includeLocal = false,
                                    size_t* playersSeen = nullptr);

// Enumerates non-player humanoid rigs (mobs, NPCs, bosses, dummies). The
// Workspace is walked incrementally in the background so the frame cost stays
// bounded even on maps with tens of thousands of parts. Mob details are
// rebuilt on a slow cadence; between rebuilds the cached positions are reused
// and only the camera distance is refreshed.
std::vector<Entity> collectMobs(const Camera& cam, size_t* mobsSeen = nullptr);

// Same detection, but walks the whole Workspace synchronously and returns the
// finished list. Only for the --scan diagnostic: it blocks for the length of a
// full walk, which is fine on a command-line dump and never in a frame.
std::vector<Entity> collectMobsNow(const Camera& cam, size_t* mobsSeen = nullptr);

Entity buildEntity(const rb::Instance& player);

// Builds an entity from a character model directly. Players pass their Player
// address; mobs leave it zero. wantTeamSig/wantTool gate the extra child walks
// so a mob scan never pays for player-only data.
Entity buildModelEntity(uintptr_t character, const std::string& name,
                        uintptr_t playerAddr, uintptr_t teamAddr,
                        bool wantTeamSig, bool wantTool);

// Character models the mob walker must never classify as a mob. Fed from the
// current player snapshot every refresh.
void setPlayerCharacters(const std::vector<uintptr_t>& models);
void setReadTools(bool on);

float readHumanoidHealth(uintptr_t humanoid, float& maxHealth);
uintptr_t localTeamAddress();
const char* boneName(int bone);

bool worldToScreen(const Camera& cam, Vec3 world, const Viewport& vp, Vec2& out);

} // namespace game
