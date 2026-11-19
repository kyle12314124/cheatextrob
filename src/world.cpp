#include "world.h"
#include "instance.h"
#include "offsets.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

// Static geometry cache behind the aimbot's "visible only" test.
//
// A live map measured ~15 500 parts, and one full Workspace walk cost ~140 ms.
// Doing that inside a frame is impossible, so the walk is spread over frames:
// each update() visits a handful of nodes until the whole service is covered,
// then keeps re-sampling the cached parts a few at a time so doors, lifts and
// moving platforms do not freeze in place. Cost stays around half a millisecond
// per frame while it works and almost nothing once it is warm.
namespace world {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int    kMaxDepth        = 8;
constexpr size_t kMaxParts        = 24000;
constexpr double kWalkBudgetMs    = 0.55;
constexpr double kRefreshBudgetMs = 0.30;
constexpr size_t kMinPerStep      = 4;
constexpr auto   kRebuildAfter    = std::chrono::seconds(12);

// Anything this close to the local body, or this close to the eye, cannot be a
// wall between the player and a target. That is the first-person weapon
// viewmodel, the player's own accessories and spawned effects - all of which are
// ordinary Workspace parts and would otherwise block every ray. Without this
// the aimbot only locks on the frames where the gun model happens to be off the
// line of fire.
constexpr float kSelfClear = 7.0f;
constexpr float kEyeClear  = 2.5f;

// A crossing thinner than this is a corner or a face the ray just grazes, not
// something a player can hide behind.
constexpr float kMinCross = 0.2f;

struct Part {
    uintptr_t part = 0;
    Vec3 center{};
    Vec3 half{};  // local half extents
    float rot[9]{};  // 3x3 from the primitive; convention checked at hit time
    Vec3 ext{};   // half extents of the world-aligned bound (early reject only)
    float bound = 0.0f;  // box diagonal; <= 0 marks a dead entry
};

struct Node {
    uintptr_t addr;
    int depth;
};

std::vector<Part> g_live;    // what segmentBlocked() reads
std::vector<Part> g_build;   // being discovered by the current walk
std::vector<Node> g_queue;
std::vector<uintptr_t> g_characters;  // player rigs; never cached as geometry
size_t g_cursor = 0;
bool   g_walking = false;
bool   g_ready = false;
size_t g_seen = 0;
size_t g_refresh = 0;
uintptr_t g_humanoidDesc = 0;
Clock::time_point g_readyAt{};
bool g_haveReadyAt = false;

// Local character head, used to ignore geometry the player is standing in:
// their own body, the first-person viewmodel and effects attached to it.
Vec3 g_selfPoint{};
bool g_haveSelf = false;

double millisSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

bool saneVec(const Vec3& v, float maxAbs) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
           std::fabs(v.x) < maxAbs && std::fabs(v.y) < maxAbs &&
           std::fabs(v.z) < maxAbs;
}

// Read one part into an occluder. Everything is validated: a half-read part
// would otherwise become a giant box that blocks the whole screen.
bool sample(uintptr_t partAddr, uintptr_t prim, Part& out) {
    if (prim < 0x10000 || prim > 0x00007FFFFFFFFFFFULL) return false;

    Vec3 pos{}, size{};
    float m[9]{};
    if (!mem::g.readRaw(prim + off::PrimitivePosition, &pos, sizeof(pos))) return false;
    if (!mem::g.readRaw(prim + off::PrimitiveSize, &size, sizeof(size))) return false;
    if (!mem::g.readRaw(prim + off::PrimitiveRotation, m, sizeof(m))) return false;

    if (!saneVec(pos, 1.0e6f)) return false;
    if (!saneVec(size, 3000.0f)) return false;
    if (size.x <= 0.0f || size.y <= 0.0f || size.z <= 0.0f) return false;

    // The rotation must look like a rotation: entries inside [-1.2, 1.2] and
    // each row roughly unit length.
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const float v = m[i * 3 + j];
            if (!std::isfinite(v) || std::fabs(v) > 1.2f) return false;
        }
        const float len2 = m[i * 3] * m[i * 3] + m[i * 3 + 1] * m[i * 3 + 1] +
                           m[i * 3 + 2] * m[i * 3 + 2];
        if (len2 < 0.5f || len2 > 1.8f) return false;
    }

    const Vec3 half{size.x * 0.5f, size.y * 0.5f, size.z * 0.5f};

    // World-aligned bound of the rotated box. Conservative for tilted parts,
    // exact for the axis-aligned walls/floor that make up most map geometry,
    // and independent of whether the 3x3 is stored row- or column-major.
    Vec3 ext{
        std::fabs(m[0]) * half.x + std::fabs(m[1]) * half.y + std::fabs(m[2]) * half.z,
        std::fabs(m[3]) * half.x + std::fabs(m[4]) * half.y + std::fabs(m[5]) * half.z,
        std::fabs(m[6]) * half.x + std::fabs(m[7]) * half.y + std::fabs(m[8]) * half.z,
    };

    // A part thinner than a hand cannot hide a player.
    if (ext.x < 0.08f && ext.y < 0.08f && ext.z < 0.08f) return false;

    out.part = partAddr;
    out.center = pos;
    out.half = half;
    for (int i = 0; i < 9; ++i) out.rot[i] = m[i];
    out.ext = ext;
    out.bound = length(ext);
    return true;
}

// Class descriptor of Humanoid is shared by every humanoid in the process, so
// a character model can be recognised with one 8-byte read per child instead
// of a name lookup per child.
uintptr_t findHumanoidDescriptor() {
    rb::Instance lp = rb::localPlayer();
    uintptr_t model = lp ? mem::g.read<uintptr_t>(lp.address() + off::PlayerModel) : 0;
    if (!model) return 0;
    for (const auto& c : rb::Instance(model).children()) {
        if (c.className() == "Humanoid")
            return mem::g.read<uintptr_t>(c.address() + off::InstanceDescriptor);
    }
    return 0;
}

bool isHumanoidModel(uintptr_t modelAddr) {
    if (!g_humanoidDesc) return false;
    for (const auto& c : rb::Instance(modelAddr).children()) {
        const uintptr_t desc = mem::g.read<uintptr_t>(c.address() + off::InstanceDescriptor);
        if (desc && desc == g_humanoidDesc) return true;
    }
    return false;
}

// Same test against children that were already read, so a deeper node costs no
// extra child enumeration. A character can be parented anywhere - a game may put
// every player under Workspace.Characters - and its own hitboxes must never end
// up in the cache.
bool hasHumanoid(const std::vector<rb::Instance>& kids) {
    if (!g_humanoidDesc) return false;
    for (const auto& c : kids) {
        const uintptr_t desc = mem::g.read<uintptr_t>(c.address() + off::InstanceDescriptor);
        if (desc && desc == g_humanoidDesc) return true;
    }
    return false;
}

bool isCharacterModel(uintptr_t addr) {
    for (const uintptr_t m : g_characters)
        if (m == addr) return true;
    return false;
}

// A rig is the only kind of object that owns a part named "HumanoidRootPart".
// Checking the children the walk already read catches corpses, NPCs and custom
// AnimationController rigs that the player list does not know about. The child
// count guard keeps the name reads off big map folders.
bool hasRigRoot(const std::vector<rb::Instance>& kids) {
    if (kids.size() < 6 || kids.size() > 120) return false;
    for (const auto& c : kids)
        if (c.name() == "HumanoidRootPart") return true;
    return false;
}

// Reads the local character's head. Re-resolved every frame because a respawn
// replaces the whole character tree.
void refreshSelf() {
    g_haveSelf = false;
    rb::Instance lp = rb::localPlayer();
    if (!lp) return;
    const uintptr_t model = mem::g.read<uintptr_t>(lp.address() + off::PlayerModel);
    if (!model) return;
    rb::Instance head = rb::Instance(model).findFirstChild("Head");
    if (!head) return;
    const uintptr_t prim = mem::g.read<uintptr_t>(head.address() + off::PartPrimitive);
    if (prim < 0x10000) return;
    Vec3 pos = mem::g.read<Vec3>(prim + off::PrimitivePosition);
    if (!saneVec(pos, 1.0e6f)) return;
    g_selfPoint = pos;
    g_haveSelf = true;
}

void visit(uintptr_t addr, int depth) {
    // A player rig, wherever the game parented it. Checking before the child
    // read also skips all of its hitboxes, accessories and weapon parts.
    if (isCharacterModel(addr)) return;

    std::vector<rb::Instance> kids = rb::Instance(addr).children();

    if (depth == 0) {
        // Characters can sit directly in Workspace. Their parts must never enter
        // the cache: a target's own head would block every ray aimed at it.
        for (const auto& c : kids) {
            if (c.className() == "Model" && isHumanoidModel(c.address())) continue;
            if (g_queue.size() < 250000) g_queue.push_back({c.address(), 1});
        }
        return;
    }

    const uintptr_t prim = mem::g.read<uintptr_t>(addr + off::PartPrimitive);
    if (prim) {
        // A node only counts as a part when the primitive actually reads like
        // one; plenty of containers carry a stray pointer-ish value at that
        // offset, and pruning those would cut whole branches out of the map.
        Part p;
        if (sample(addr, prim, p)) {
            if (g_build.size() < kMaxParts) {
                g_build.push_back(p);
                ++g_seen;
            }
            // Parts are leaves for our purposes; walking their attachments and
            // decals only burns reads.
            return;
        }
    }

    // Deeper down the same rule: any model that owns a Humanoid is a character,
    // wherever the game chose to parent it. Rigs with an AnimationController
    // have no Humanoid, so fall back to the presence of a HumanoidRootPart.
    if (hasHumanoid(kids) || hasRigRoot(kids)) return;

    if (kids.empty() || depth >= kMaxDepth) return;
    for (const auto& c : kids) {
        if (g_queue.size() < 250000) g_queue.push_back({c.address(), depth + 1});
    }
}

bool startWalk() {
    rb::Instance ws = rb::workspace();
    if (!ws) return false;

    // The Humanoid descriptor is only a fallback: characters are normally
    // identified from the player list (setCharacters). Custom rigs such as the
    // AnimationController-based ones in FPS games have no Humanoid at all, and a
    // missing descriptor must not stop the walk - that would make "visible only"
    // silently do nothing.
    if (!g_humanoidDesc) g_humanoidDesc = findHumanoidDescriptor();

    g_build.clear();
    g_build.reserve(g_live.size() + 1024);
    g_queue.clear();
    g_queue.reserve(16384);
    g_queue.push_back({ws.address(), 0});
    g_cursor = 0;
    g_seen = 0;
    g_walking = true;
    return true;
}

void walkStep() {
    const auto t0 = Clock::now();
    size_t done = 0;
    while (g_cursor < g_queue.size()) {
        const Node n = g_queue[g_cursor++];
        visit(n.addr, n.depth);
        ++done;
        if (done >= kMinPerStep && millisSince(t0) >= kWalkBudgetMs) break;
        if (g_build.size() >= kMaxParts) break;
    }

    if (g_cursor < g_queue.size() && g_build.size() < kMaxParts) return;

    // Swap only once the walk is complete, so the previous snapshot keeps
    // answering ray tests while the new one is being built.
    g_live.swap(g_build);
    g_build.clear();
    g_queue.clear();
    g_cursor = 0;
    g_walking = false;
    g_ready = true;
    g_refresh = 0;
    g_readyAt = Clock::now();
    g_haveReadyAt = true;
}

void refreshStep() {
    if (g_live.empty()) return;
    if (g_refresh >= g_live.size()) {
        g_refresh = 0;
        return;
    }

    const auto t0 = Clock::now();
    size_t done = 0;
    while (g_refresh < g_live.size()) {
        Part& p = g_live[g_refresh++];
        const uintptr_t prim = mem::g.read<uintptr_t>(p.part + off::PartPrimitive);
        Part fresh;
        if (sample(p.part, prim, fresh))
            p = fresh;
        else
            p.bound = -1.0f;
        ++done;
        if (done >= kMinPerStep && millisSince(t0) >= kRefreshBudgetMs) break;
    }
}

} // namespace

void setCharacters(const std::vector<uintptr_t>& models) {
    // Called every frame with the current player list; the comparison keeps the
    // common case free and the walk sees a stable set while it is running.
    if (models == g_characters) return;
    g_characters = models;
}

void reset() {
    g_live.clear();
    g_build.clear();
    g_queue.clear();
    g_cursor = 0;
    g_walking = false;
    g_ready = false;
    g_seen = 0;
    g_refresh = 0;
    g_humanoidDesc = 0;
    g_haveReadyAt = false;
    g_haveSelf = false;
}

void update(bool active) {
    // Cheap per-frame read; also keeps the eye/self exemption correct after a
    // respawn or a teleport.
    refreshSelf();

    if (!mem::g.valid()) {
        if (!g_live.empty()) reset();
        return;
    }

    if (g_walking) {
        walkStep();
        return;
    }

    if (!g_ready) {
        if (startWalk()) walkStep();
        return;
    }

    if (g_haveReadyAt && Clock::now() - g_readyAt > kRebuildAfter) {
        if (startWalk()) walkStep();
        return;
    }

    // Re-sampling only matters while a lock is being tested; between locks the
    // cache is refreshed by the periodic rebuild instead of every frame.
    if (active) refreshStep();
}

// Slab test against a box centred on the origin, in that box's own axes.
bool slabHit(const float o[3], const float d[3], const float h[3], float& t) {
    float tmin = 0.0f, tmax = 1.0f;
    for (int a = 0; a < 3; ++a) {
        if (std::fabs(d[a]) < 1e-5f) {
            if (std::fabs(o[a]) > h[a]) return false;
            continue;
        }
        float t1 = (-h[a] - o[a]) / d[a];
        float t2 = (h[a] - o[a]) / d[a];
        if (t1 > t2) std::swap(t1, t2);
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        if (tmin > tmax) return false;
    }
    // A crossing right at the target is their own body or the wall they hug.
    if (tmin <= 0.0001f || tmin >= 0.985f) return false;
    // Grazing a corner is not cover.
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if ((tmax - tmin) * len < kMinCross) return false;
    t = tmin;
    return true;
}

// Distance from a world point to a part's world-aligned bound.
float boxDistance(const Part& p, const Vec3& pt) {
    const Vec3 rel = pt - p.center;
    const float dx = std::max(0.0f, std::fabs(rel.x) - p.ext.x);
    const float dy = std::max(0.0f, std::fabs(rel.y) - p.ext.y);
    const float dz = std::max(0.0f, std::fabs(rel.z) - p.ext.z);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// True when the segment crosses this part's actual (rotated) box; t is the
// entry parameter.
//
// The 3x3 in the primitive is not documented as row- or column-major, so both
// readings are tried. For axis-aligned parts - most walls, floors and crates -
// the two are identical and the test is exact; tilted props no longer block
// through their inflated bounding box.
//
// A part whose world bound contains either endpoint never counts as an
// occluder. That covers the third-person camera clipping into geometry, game
// parts that sit on the camera (first-person hitboxes), and players standing
// inside a hollow mesh. It uses the conservative bound on purpose: a part the
// camera is inside must never be allowed to blanket every ray.
bool crosses(const Part& p, const Vec3& from, const Vec3& seg, float& t) {
    const Vec3 rel = from - p.center;
    const Vec3 endRel = rel + seg;

    if (std::fabs(rel.x) <= p.ext.x && std::fabs(rel.y) <= p.ext.y &&
        std::fabs(rel.z) <= p.ext.z)
        return false;
    if (std::fabs(endRel.x) <= p.ext.x && std::fabs(endRel.y) <= p.ext.y &&
        std::fabs(endRel.z) <= p.ext.z)
        return false;

    // The first-person weapon viewmodel is a real part a stud or two from the
    // eye, and accessories and effects spawn on the player's own body. None of
    // that is cover, but all of it would otherwise block every ray.
    if (g_haveSelf && boxDistance(p, g_selfPoint) < kSelfClear) return false;
    if (boxDistance(p, from) < kEyeClear) return false;

    const float h[3] = {p.half.x, p.half.y, p.half.z};
    bool hitAny = false;
    float hitT = 0.0f;

    for (int conv = 0; conv < 2; ++conv) {
        const float* m = p.rot;
        float o[3], d[3];
        for (int i = 0; i < 3; ++i) {
            if (conv == 0) {
                o[i] = m[i] * rel.x + m[3 + i] * rel.y + m[6 + i] * rel.z;
                d[i] = m[i] * seg.x + m[3 + i] * seg.y + m[6 + i] * seg.z;
            } else {
                o[i] = m[i * 3] * rel.x + m[i * 3 + 1] * rel.y + m[i * 3 + 2] * rel.z;
                d[i] = m[i * 3] * seg.x + m[i * 3 + 1] * seg.y + m[i * 3 + 2] * seg.z;
            }
        }

        float candidate = 0.0f;
        if (slabHit(o, d, h, candidate) && (!hitAny || candidate < hitT)) {
            hitAny = true;
            hitT = candidate;
        }
    }

    if (!hitAny) return false;
    t = hitT;
    return true;
}

bool segmentBlocked(Vec3 from, Vec3 to) {
    if (g_live.empty()) return false;

    const Vec3 seg = to - from;
    const float len = length(seg);
    if (len < 0.05f) return false;
    const Vec3 dir = seg * (1.0f / len);

    for (const auto& p : g_live) {
        if (p.bound <= 0.0f) continue;

        // Sphere reject: most parts are nowhere near the ray, and rejecting
        // them here costs a dot product instead of a slab test.
        const Vec3 oc = p.center - from;
        const float along = dot(oc, dir);
        if (along < -p.bound || along > len + p.bound) continue;
        const float perp2 = dot(oc, oc) - along * along;
        if (perp2 > p.bound * p.bound) continue;

        float t = 0.0f;
        if (crosses(p, from, seg, t)) return true;
    }
    return false;
}

bool firstBlocker(Vec3 from, Vec3 to, Vec3* center, Vec3* ext, float* t,
                  uintptr_t* part) {
    if (g_live.empty()) return false;
    const Vec3 seg = to - from;
    const float len = length(seg);
    if (len < 0.05f) return false;
    const Vec3 dir = seg * (1.0f / len);

    float best = 2.0f;
    const Part* hit = nullptr;
    for (const auto& p : g_live) {
        if (p.bound <= 0.0f) continue;

        const Vec3 oc = p.center - from;
        const float along = dot(oc, dir);
        if (along < -p.bound || along > len + p.bound) continue;
        const float perp2 = dot(oc, oc) - along * along;
        if (perp2 > p.bound * p.bound) continue;

        float hitT = 0.0f;
        if (!crosses(p, from, seg, hitT)) continue;
        if (hitT < best) {
            best = hitT;
            hit = &p;
        }
    }
    if (!hit) return false;
    if (center) *center = hit->center;
    if (ext) *ext = hit->ext;
    if (t) *t = best;
    if (part) *part = hit->part;
    return true;
}

void buildNow() {
    if (!mem::g.valid()) return;
    reset();
    refreshSelf();
    startWalk();
    while (g_walking) walkStep();
}

size_t occluders() {
    size_t n = 0;
    for (const auto& p : g_live)
        if (p.bound > 0.0f) ++n;
    return n;
}

size_t partsSeen() { return g_seen; }

bool ready() { return g_ready; }

} // namespace world
