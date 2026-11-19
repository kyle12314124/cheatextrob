#include "game.h"
#include "offsets.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace game {

namespace {
constexpr size_t kMaxPlayers = 256;

Vec3 readPartSize(uintptr_t part) {
    uintptr_t prim = mem::g.read<uintptr_t>(part + off::PartPrimitive);
    if (!prim) return {};
    float s[3]{};
    if (!mem::g.readRaw(prim + off::PrimitiveSize, s, sizeof(s))) return {};
    if (!(s[0] > 0.05f && s[0] < 50.0f) ||
        !(s[1] > 0.05f && s[1] < 50.0f) ||
        !(s[2] > 0.05f && s[2] < 50.0f)) return {};
    return {s[0], s[1], s[2]};
}

bool saneSize(Vec3 s) { return s.y > 0.05f && s.y < 50.0f; }

// A character the client is still simulating lives under Workspace. Games that
// stream players out (Bloxstrike parks far characters under
// ReplicatedStorage._PVS_CulledCharacters, and other games park them at a
// staging coordinate) leave a copy whose transform is frozen, so drawing it
// just paints a box in the wrong place - the "ESP stuck to one spot until the
// enemy comes into view" bug. Everything the client actually tracks is a
// Workspace descendant; if the Workspace cannot be read, keep the entity
// rather than blanking the whole ESP.
bool inWorkspace(uintptr_t model, uintptr_t ws) {
    if (!model) return false;
    if (!ws) return true;
    rb::Instance cur(model);
    for (int i = 0; i < 12 && cur; ++i) {
        if (cur.address() == ws) return true;
        cur = cur.parent();
    }
    return false;
}

// Kit containers that identify a side in games that do not use Roblox teams.
// Names are matched against the container itself so weapons and effects - which
// change per player - never enter the signature.
bool isKitContainer(const std::string& name, const std::string& cls) {
    if (name == "WeaponModel" || name == "WeaponAttachments" || name == "Highlight" ||
        name == "CameraPart")
        return false;
    return cls == "Model" || cls == "Folder" || cls == "Accessory";
}

// Build the sorted, deduplicated uniform signature from an already-fetched
// child list. Doing it from the same list buildEntity walks means a player who
// is still loading (or who picks up armour mid-round) never gets a stale or
// half-filled signature.
std::string kitSignature(const std::vector<rb::Instance>& kids) {
    std::vector<std::string> names;
    for (const auto& c : kids) {
        if (!isKitContainer(c.name(), c.className())) continue;
        for (const auto& g : rb::Instance(c.address()).children()) {
            const std::string gn = g.name();
            if (!gn.empty()) names.push_back(gn);
        }
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());

    std::string sig = "|";
    for (const auto& n : names) {
        sig += n;
        sig += '|';
    }
    return sig;
}
} // namespace

std::string teamSignature(uintptr_t character) {
    if (!character) return {};
    return kitSignature(rb::Instance(character).children());
}

const char* boneName(int bone) {
    static const char* const kNames[BoneCount] = {
        "head", "upperTorso", "lowerTorso",
        "leftUpperArm", "leftLowerArm", "leftHand",
        "rightUpperArm", "rightLowerArm", "rightHand",
        "leftUpperLeg", "leftLowerLeg", "leftFoot",
        "rightUpperLeg", "rightLowerLeg", "rightFoot",
        "torso", "leftArm", "rightArm", "leftLeg", "rightLeg",
    };
    return (bone >= 0 && bone < BoneCount) ? kNames[bone] : "?";
}

uintptr_t readPrimitive(uintptr_t part) {
    if (!part) return 0;
    return mem::g.read<uintptr_t>(part + off::PartPrimitive);
}

Vec3 readPartPosition(uintptr_t part) {
    uintptr_t prim = readPrimitive(part);
    if (!prim) return {};
    float pos[3]{};
    if (!mem::g.readRaw(prim + off::PrimitivePosition, pos, sizeof(pos)))
        return {};
    return {pos[0], pos[1], pos[2]};
}

bool readCamera(Camera& out) {
    rb::Instance cam = rb::camera();
    if (!cam) return false;
    const uintptr_t addr = cam.address();

    // The game rewrites the camera CFrame every rendered frame. Reading those
    // 36 bytes while it does gives a mix of old and new basis vectors: the
    // result still projects "somewhere sane" but is off for a single frame, so
    // the ESP pops while you turn the camera. Reduce to a double read and only
    // trust a value that did not change between two reads; otherwise reuse the
    // last good basis, which is one frame behind and invisible.
    static Camera last{};
    static uintptr_t lastAddr = 0;
    static bool haveLast = false;
    if (addr != lastAddr) { haveLast = false; lastAddr = addr; }

    float m[9]{}, m2[9]{};
    bool stable = false;
    for (int attempt = 0; attempt < 4 && !stable; ++attempt) {
        if (!mem::g.readRaw(addr + off::CameraRotation, m, sizeof(m))) return false;
        if (!mem::g.readRaw(addr + off::CameraRotation, m2, sizeof(m2))) return false;
        stable = std::memcmp(m, m2, sizeof(m)) == 0;
    }
    if (!stable) {
        if (!haveLast) return false;
        out = last;
        return true;
    }

    // Verified live on version-4ea8e85481e34f46: the stored 3x3 has the basis
    // vectors in its COLUMNS, i.e. right=(m0,m3,m6), up=(m1,m4,m7),
    // back=(m2,m5,m8). Proof from a live frame:
    //   right.y == 0 exactly (Roblox cameras never roll), while reading the
    //   rows as basis gives right.y = -0.185, and -(m2,m5,m8) equals the
    //   ViewMatrix's forward row exactly.
    // Do NOT try to auto-detect this per frame: a rotation matrix and its
    // transpose are both orthonormal, so the "error" test is a coin flip and
    // the basis flickers between them (ESP jumps when you turn).
    Vec3 right{m[0], m[3], m[6]};
    Vec3 up   {m[1], m[4], m[7]};
    Vec3 back {m[2], m[5], m[8]};

    const bool finite = std::isfinite(right.x) && std::isfinite(right.y) &&
                        std::isfinite(right.z) && std::isfinite(up.x) &&
                        std::isfinite(up.y) && std::isfinite(up.z) &&
                        std::isfinite(back.x) && std::isfinite(back.y) &&
                        std::isfinite(back.z);
    if (!finite) {
        if (!haveLast) return false;
        out = last;
        return true;
    }
    if (length(right) < 0.5f || length(right) > 1.5f ||
        length(up) < 0.5f || length(up) > 1.5f) {
        if (!haveLast) return false;
        out = last;
        return true;
    }

    out.right = normalize(right);
    out.up    = normalize(up);
    out.look  = normalize(back) * -1.0f;

    float pos[3]{};
    if (!mem::g.readRaw(addr + off::CameraPosition, pos, sizeof(pos))) {
        if (!haveLast) return false;
        out = last;
        return true;
    }
    if ((std::fabs(pos[0]) + std::fabs(pos[1]) + std::fabs(pos[2])) > 2.0e7f) {
        if (!haveLast) return false;
        out = last;
        return true;
    }
    out.pos = {pos[0], pos[1], pos[2]};

    float fov = mem::g.read<float>(addr + off::CameraFOV);
    // Roblox stores FieldOfView in radians (~1.22 for the default 70 degrees).
    if (fov > 0.1f && fov < 3.2f) fov = fov * (180.0f / 3.14159265358979323846f);
    out.fov = (fov > 1.0f && fov < 179.0f) ? fov : (haveLast ? last.fov : 70.0f);

    last = out;
    haveLast = true;
    return true;
}

float readHumanoidHealth(uintptr_t humanoid, float& maxHealth) {
    maxHealth = 100.0f;
    if (!humanoid) return 0.0f;
    float h  = mem::g.read<float>(humanoid + off::HumanoidHealth);
    float mh = mem::g.read<float>(humanoid + off::HumanoidMaxHealth);
    if (mh > 0.0f && mh < 100000.0f) maxHealth = mh;
    if (h < 0.0f || h > 100000.0f) h = 0.0f;
    return h;
}

uintptr_t localTeamAddress() {
    rb::Instance lp = rb::localPlayer();
    if (!lp) return 0;
    return mem::g.read<uintptr_t>(lp.address() + off::PlayerTeam);
}

// Character part name -> joint. R15 uses the split limb names, R6 the classic
// "Left Arm" style names; both are resolved in the same single child pass.
int boneFromPartName(const std::string& n) {
    if (n == "Head") return BoneHead;
    if (n == "UpperTorso") return BoneUpperTorso;
    if (n == "LowerTorso") return BoneLowerTorso;
    if (n == "LeftUpperArm") return BoneLeftUpperArm;
    if (n == "LeftLowerArm") return BoneLeftLowerArm;
    if (n == "LeftHand") return BoneLeftHand;
    if (n == "RightUpperArm") return BoneRightUpperArm;
    if (n == "RightLowerArm") return BoneRightLowerArm;
    if (n == "RightHand") return BoneRightHand;
    if (n == "LeftUpperLeg") return BoneLeftUpperLeg;
    if (n == "LeftLowerLeg") return BoneLeftLowerLeg;
    if (n == "LeftFoot") return BoneLeftFoot;
    if (n == "RightUpperLeg") return BoneRightUpperLeg;
    if (n == "RightLowerLeg") return BoneRightLowerLeg;
    if (n == "RightFoot") return BoneRightFoot;
    if (n == "Torso") return BoneTorso;
    if (n == "Left Arm" || n == "LeftArm") return BoneLeftArm;
    if (n == "Right Arm" || n == "RightArm") return BoneRightArm;
    if (n == "Left Leg" || n == "LeftLeg") return BoneLeftLeg;
    if (n == "Right Leg" || n == "RightLeg") return BoneRightLeg;
    return -1;
}

Entity buildEntity(const rb::Instance& player) {
    Entity e;
    if (!player) return e;

    e.playerAddr = player.address();
    e.name = player.name();
    e.teamAddr = mem::g.read<uintptr_t>(player.address() + off::PlayerTeam);

    uintptr_t character = mem::g.read<uintptr_t>(player.address() + off::PlayerModel);
    if (!character) return e;
    e.characterAddr = character;

    // One pass over the character's children. findFirstChild() per part walked
    // the whole child list every time (4 walks); this walks it once and also
    // yields every joint the skeleton needs and the uniform signature.
    auto kids = rb::Instance(character).children();

    // The uniform signature is stable for the life of a character, so cache it
    // by address and only rebuild when the child count changes (respawn into a
    // new body, or the rig still loading). Keyed on the count so a recycled
    // address with a different rig is never trusted.
    static std::unordered_map<uintptr_t, std::pair<size_t, std::string>> kitCache;
    auto kitIt = kitCache.find(character);
    if (kitIt != kitCache.end() && kitIt->second.first == kids.size()) {
        e.teamSig = kitIt->second.second;
    } else {
        e.teamSig = kitSignature(kids);
        if (kitCache.size() > 1024) kitCache.clear();
        kitCache[character] = {kids.size(), e.teamSig};
    }

    uintptr_t partAddr[BoneCount]{};
    uintptr_t humanoid = 0;
    uintptr_t root = 0;
    for (const auto& c : kids) {
        std::string n = c.name();
        if (n.empty()) continue;
        if (n == "Humanoid") { humanoid = c.address(); continue; }
        if (n == "HumanoidRootPart") { root = c.address(); continue; }
        int b = boneFromPartName(n);
        if (b >= 0) partAddr[b] = c.address();
    }

    e.humanoidAddr = humanoid;
    e.headAddr = partAddr[BoneHead];
    e.rootAddr = root;
    e.torsoAddr = partAddr[BoneUpperTorso] ? partAddr[BoneUpperTorso] : partAddr[BoneTorso];
    e.r15 = partAddr[BoneUpperTorso] != 0 || partAddr[BoneLowerTorso] != 0;

    if (e.humanoidAddr)
        e.health = readHumanoidHealth(e.humanoidAddr, e.maxHealth);

    bool haveFeet = false;
    float feetY = 0.0f;
    for (int i = 0; i < BoneCount; ++i) {
        if (!partAddr[i]) continue;
        Vec3 p = readPartPosition(partAddr[i]);
        if (length(p) < 1e-3f) continue;
        e.bones[i] = p;
        e.boneMask |= 1u << i;

        // Find the lowest point of the legs/feet for the box bottom. R6 has
        // Humanoid.HipHeight == 0, which is why a root-based box stopped at the
        // thighs; the leg parts themselves always reach the ground.
        const bool leg = (i == BoneLeftFoot || i == BoneRightFoot ||
                          i == BoneLeftLeg || i == BoneRightLeg ||
                          i == BoneLeftLowerLeg || i == BoneRightLowerLeg);
        if (!leg) continue;
        Vec3 s = readPartSize(partAddr[i]);
        float half = saneSize(s) ? s.y * 0.5f
                                 : ((i == BoneLeftFoot || i == BoneRightFoot) ? 0.5f : 1.0f);
        float bottom = p.y - half;
        if (!haveFeet || bottom < feetY) { feetY = bottom; haveFeet = true; }
    }

    e.headPos  = e.bones[BoneHead];
    e.rootPos  = readPartPosition(e.rootAddr);
    e.torsoPos = e.torsoAddr ? e.bones[BoneUpperTorso] : e.bones[BoneTorso];
    if (length(e.torsoPos) < 1e-3f) e.torsoPos = readPartPosition(e.torsoAddr);
    if (length(e.headPos) < 1e-3f) e.headPos = e.torsoPos;
    if (length(e.rootPos) < 1e-3f) e.rootPos = e.torsoPos;

    // Exact vertical extent: top of the head to the soles of the feet.
    Vec3 headSize = readPartSize(e.headAddr);
    e.boxTop = e.headPos;
    if (saneSize(headSize)) e.boxTop.y += headSize.y * 0.5f;

    if (haveFeet) {
        e.boxBottom = {e.rootPos.x, feetY, e.rootPos.z};
    } else {
        Vec3 rootSize = readPartSize(e.rootAddr);
        float hipHeight = e.humanoidAddr
                              ? mem::g.read<float>(e.humanoidAddr + off::HumanoidHipHeight)
                              : 0.0f;
        if (!(hipHeight > 0.0f && hipHeight < 12.0f)) hipHeight = 0.0f;
        e.boxBottom = e.rootPos;
        if (saneSize(rootSize) || hipHeight > 0.0f)
            e.boxBottom.y -= (saneSize(rootSize) ? rootSize.y * 0.5f : 0.0f) + hipHeight;
        else
            e.boxBottom.y = e.rootPos.y - (e.headPos.y - e.rootPos.y);
    }

    // Half extents for the 3D box: how far the body parts reach from the root.
    float radius = 0.75f;
    if (e.boneMask)
        for (int i = 0; i < BoneCount; ++i) {
            if (!(e.boneMask & (1u << i))) continue;
            float dx = e.bones[i].x - e.rootPos.x, dz = e.bones[i].z - e.rootPos.z;
            radius = std::max(radius, std::sqrt(dx * dx + dz * dz) * 0.8f);
        }
    radius = std::clamp(radius + 0.2f, 0.9f, 4.0f);
    e.boxHalf = {radius, 0.0f, radius * 0.7f};
    return e;
}

std::vector<Entity> collectEntities(const Camera& cam, bool includeLocal, size_t* playersSeen) {
    std::vector<Entity> out;
    if (playersSeen) *playersSeen = 0;
    rb::Instance players = rb::players();
    if (!players) return out;

    uintptr_t localAddr = rb::localPlayer().address();
    const uintptr_t wsAddr = rb::workspace().address();

    auto children = players.children();
    if (children.size() > kMaxPlayers) children.resize(kMaxPlayers);
    if (playersSeen) *playersSeen = children.size();

    for (const auto& p : children) {
        if (!includeLocal && p.address() == localAddr) continue;
        if (p.className() != "Player") continue;

        std::string n = p.name();
        if (n.empty()) continue;

        Entity e = buildEntity(p);
        if (!e.characterAddr || !e.headAddr) continue;
        if (e.health <= 0.0f) continue;
        // Streamed-out characters are parked outside Workspace with a frozen
        // transform. Keep them: the copy still holds the last place the client
        // saw that player, so a far enemy stays on the ESP instead of vanishing
        // the moment they leave the PVS. Mark it so the aimbot does not lock a
        // position that is no longer live.
        e.ghost = !inWorkspace(e.characterAddr, wsAddr);
        // A custom rig (no Humanoid) that is lying flat is a corpse: this game
        // keeps no health to read, so head-to-feet extent is the signal. Only
        // apply it when there is no Humanoid, so a crouching or swimming live
        // player on a normal rig is never mistaken for a body.
        if (!e.humanoidAddr && e.boxTop.y - e.boxBottom.y < 2.0f) continue;
        // refuse entities whose part reads came back empty: they would draw at
        // the world origin rather than on the character
        if (length(e.headPos) < 1e-3f && length(e.rootPos) < 1e-3f) continue;

        e.distance = length(e.rootPos - cam.pos);
        out.push_back(std::move(e));
    }
    return out;
}

namespace {
struct WindowSearch {
    DWORD pid;
    HWND best;
    long area;
};

BOOL CALLBACK enumWindow(HWND h, LPARAM lp) {
    auto* s = reinterpret_cast<WindowSearch*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != s->pid || !IsWindowVisible(h)) return TRUE;
    RECT rc{};
    if (!GetClientRect(h, &rc)) return TRUE;
    long area = (rc.right - rc.left) * (rc.bottom - rc.top);
    if (area > s->area) {
        s->area = area;
        s->best = h;
    }
    return TRUE;
}
} // namespace

Viewport robloxViewport() {
    Viewport vp{};

    static HWND cached = nullptr;
    HWND hwnd = cached;
    if (!hwnd || !IsWindow(hwnd)) {
        hwnd = FindWindowW(off::WindowClass, nullptr);
        if (!hwnd) {
            WindowSearch s{static_cast<DWORD>(mem::g.pid()), nullptr, 0};
            EnumWindows(enumWindow, reinterpret_cast<LPARAM>(&s));
            hwnd = s.best;
        }
        cached = hwnd;
    }

    if (hwnd) {
        RECT rc{};
        if (GetClientRect(hwnd, &rc)) {
            POINT tl{rc.left, rc.top};
            ClientToScreen(hwnd, &tl);
            vp.x = static_cast<float>(tl.x);
            vp.y = static_cast<float>(tl.y);
            vp.w = static_cast<float>(rc.right - rc.left);
            vp.h = static_cast<float>(rc.bottom - rc.top);

            // The rendered viewport can be smaller than the window (letterbox,
            // window chrome). Prefer the camera's own viewport size.
            rb::Instance cam = rb::camera();
            if (cam) {
                float vw = mem::g.read<float>(cam.address() + off::CameraViewportSize);
                float vh = mem::g.read<float>(cam.address() + off::CameraViewportSize + 4);
                if (vw > 16.0f && vh > 16.0f && vw <= vp.w + 1.0f && vh <= vp.h + 1.0f &&
                    (vw < vp.w - 1.0f || vh < vp.h - 1.0f)) {
                    vp.x += (vp.w - vw) * 0.5f;
                    vp.y += (vp.h - vh) * 0.5f;
                    vp.w = vw;
                    vp.h = vh;
                }
            }
        }
    }

    if (vp.w <= 0.0f || vp.h <= 0.0f) {
        vp.x = 0.0f;
        vp.y = 0.0f;
        vp.w = static_cast<float>(GetSystemMetrics(SM_CXSCREEN));
        vp.h = static_cast<float>(GetSystemMetrics(SM_CYSCREEN));
    }
    return vp;
}

bool worldToScreen(const Camera& cam, Vec3 world, const Viewport& vp, Vec2& out) {
    Vec3 rel = world - cam.pos;
    float z = dot(rel, cam.look);
    if (z <= 0.1f) return false;

    constexpr float kPi = 3.14159265358979323846f;
    float tanHalf = std::tan(cam.fov * 0.5f * kPi / 180.0f);
    if (tanHalf < 1e-4f) return false;

    float width = vp.w;
    float height = vp.h;
    float aspect = (height > 0.0f) ? (width / height) : 1.0f;
    float sx = (dot(rel, cam.right) / (z * tanHalf * aspect)) * (width * 0.5f) + width * 0.5f;
    float sy = -(dot(rel, cam.up)  / (z * tanHalf))           * (height * 0.5f) + height * 0.5f;
    out = {vp.x + sx, vp.y + sy};
    return true;
}

} // namespace game
