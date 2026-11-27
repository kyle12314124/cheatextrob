#include "features.h"
#include "game.h"
#include "instance.h"
#include "offsets.h"
#include "overlay.h"
#include "render.h"
#include "ui.h"
#include "world.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace feat {

Settings g;

namespace {

// ---- footer toast ----
char g_toast[64] = {};
ULONGLONG g_toastUntil = 0;

// ---- frame timing ----
ULONGLONG g_fpsStart = 0;
float  g_fps = 0.0f;
int    g_fpsFrames = 0;

// ---- per-frame cache (update fills it, render consumes it) ----
bool  g_camValid = false;
game::Camera g_cam{};
std::vector<game::Entity> g_entities;   // players
std::vector<game::Entity> g_mobs;       // non-player rigs
std::vector<game::Entity> g_all;        // players + mobs, for draw/aim
uintptr_t g_localTeam = 0;

// ---- diag (--diag): proves whether blank ESP frames are our fault or the
// compositor's. One line per second in diag.txt. ----
bool g_diag = false;
ULONGLONG g_diagStart = 0;
int g_diagFrames = 0, g_diagBoxFrames = 0, g_diagCamFail = 0;
int g_diagBoxes = 0, g_diagEntMin = 1 << 30, g_diagEntMax = 0;
int g_diagEntDrop = 0;
int g_lastEntCount = -1;
int g_keepStale = 0;
float g_diagCostMs = 0.0f;
float g_diagAimMs = 0.0f;

void diagTick(bool camOk, int boxes, int ents) {
    if (!g_diag) return;
    ++g_diagFrames;
    if (!camOk) ++g_diagCamFail;
    if (boxes > 0) ++g_diagBoxFrames;
    g_diagBoxes += boxes;
    g_diagEntMin = std::min(g_diagEntMin, ents);
    g_diagEntMax = std::max(g_diagEntMax, ents);

    ULONGLONG now = GetTickCount64();
    if (!g_diagStart) g_diagStart = now;
    if (now - g_diagStart < 1000) return;

    FILE* f = nullptr;
    fopen_s(&f, "diag.txt", "a");
    if (f) {
        std::fprintf(f, "t=%4llus frames=%3d esp=%3d camfail=%2d boxes=%4d ents=%d..%d drop=%d fps=%.0f walk=%.1fms aim=%.2fms occ=%zu\n",
                     (now - g_diagStart) / 1000, g_diagFrames, g_diagBoxFrames, g_diagCamFail,
                     g_diagBoxes, g_diagEntMin, g_diagEntMax, g_diagEntDrop, g_fps, g_diagCostMs,
                     g_diagAimMs, world::occluders());
        std::fclose(f);
    }
    g_diagFrames = g_diagBoxFrames = g_diagCamFail = 0;
    g_diagBoxes = 0;
    g_diagEntMin = 1 << 30;
    g_diagEntMax = 0;
    g_diagEntDrop = 0;
    g_diagStart = now;
}

// ---- aim state ----
std::unordered_map<uintptr_t, Vec3> g_prevPos;
bool g_aimHeldPrev = false;   // key state last frame, for the toggle edge
bool g_aimToggled = false;    // latched state in toggle mode

// One call per frame: applies hold/toggle and returns whether the aimbot should
// run. Toggle flips on the key's rising edge, so one tap arms it and the next
// tap disarms it; hold is simply key-down.
bool aimActive() {
    if (!g.aim) {
        g_aimHeldPrev = false;
        g_aimToggled = false;
        return false;
    }
    const bool down = (GetAsyncKeyState(g.aimKey) & 0x8000) != 0;
    if (g.aimMode == 1) {
        if (down && !g_aimHeldPrev) g_aimToggled = !g_aimToggled;
        g_aimHeldPrev = down;
        return g_aimToggled;
    }
    g_aimHeldPrev = down;
    g_aimToggled = false;
    return down;
}

// ---- helpers ----

uint32_t argbOf(const float c[4]) {
    auto clamp255 = [](float v) {
        int i = static_cast<int>(v * 255.0f + 0.5f);
        return static_cast<uint32_t>(std::clamp(i, 0, 255));
    };
    return (clamp255(c[3]) << 24) | (clamp255(c[0]) << 16) |
           (clamp255(c[1]) << 8) | clamp255(c[2]);
}

uint32_t withAlpha(uint32_t argb, float a) {
    return (argb & 0x00FFFFFF) |
           (static_cast<uint32_t>(255.0f * std::clamp(a, 0.0f, 1.0f)) << 24);
}

uint32_t lighten(uint32_t argb, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto ch = [&](int shift) {
        uint32_t v = (argb >> shift) & 0xFFu;
        return static_cast<uint32_t>(v + (255u - v) * t);
    };
    return (argb & 0xFF000000u) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

void textShadow(float x, float y, const char* s, uint32_t col, float size, bool bold = false) {
    rnd::text(x + 1.0f, y + 1.0f, s, 0xC0000000, size, bold);
    rnd::text(x, y, s, col, size, bold);
}

// Text that stays readable on any scene: four dark offsets and the glyphs on
// top. Used for names, where a plate would only add clutter.
void textOutlined(float x, float y, const char* s, uint32_t col, float size, bool bold = false) {
    const uint32_t outline = 0xCC000000;
    rnd::text(x - 1.0f, y, s, outline, size, bold);
    rnd::text(x + 1.0f, y, s, outline, size, bold);
    rnd::text(x, y - 1.0f, s, outline, size, bold);
    rnd::text(x, y + 1.0f, s, outline, size, bold);
    rnd::text(x, y, s, col, size, bold);
}

void sendMouseMove(float dx, float dy) {
    if (std::fabs(dx) < 0.5f && std::fabs(dy) < 0.5f) return;
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = static_cast<LONG>(std::lround(dx));
    in.mi.dy = static_cast<LONG>(std::lround(dy));
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(INPUT));
}

void sendClick() {
    INPUT in[2]{};
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, in, sizeof(INPUT));
}

bool keyDown(int vk) {
    if (!vk) return false;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// Zone comparison for games that do use Roblox teams, with a kit-signature
// fallback for games that don't (players on one side wear the same armour and
// accessories). The two sides in a game like this have visibly different
// uniforms but still share a belt and gloves, so the test is an overlap ratio:
// 50 % keeps a teammate who dropped a glove or picked up a vest on-side while
// never bridging two uniforms that share only the common base pieces.
bool sameKit(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;

    int common = 0, total = 0;
    size_t i = 1;
    while (i < a.size()) {
        const size_t j = a.find('|', i);
        if (j == std::string::npos) break;
        const std::string tok = a.substr(i, j - i);
        ++total;
        if (b.find("|" + tok + "|") != std::string::npos) ++common;
        i = j + 1;
    }
    size_t k = 1;
    while (k < b.size()) {
        const size_t j = b.find('|', k);
        if (j == std::string::npos) break;
        const std::string tok = b.substr(k, j - k);
        if (a.find("|" + tok + "|") == std::string::npos) ++total;
        k = j + 1;
    }
    return total > 0 && common * 10 >= total * 5;
}

std::string g_localTeamSig;  // kit of the local player's side
bool g_headSplit = false;    // two distinct head-gear sides are present

// Bloxstrike-style uniforms: the two sides wear different headgear, while armour
// (helmet, vest detail, pads) is bought on top and can appear on either side. A
// full-set overlap is therefore noisy - an enemy who bought a helmet starts to
// look like a teammate. The head piece is stable for the whole side, so it wins
// when both sides are actually present in the snapshot; otherwise the kit
// overlap is used.
int headSide(const std::string& sig) {
    if (sig.empty()) return 0;
    if (sig.find("|Balaclava|") != std::string::npos) return 1;
    if (sig.find("|HeadCover|") != std::string::npos) return 2;
    return 0;
}

bool sameTeam(uintptr_t localTeam, const game::Entity& e) {
    if (localTeam && e.teamAddr) return localTeam == e.teamAddr;
    if (g_headSplit) {
        const int ls = headSide(g_localTeamSig), es = headSide(e.teamSig);
        if (ls && es) return ls == es;
    }
    return sameKit(g_localTeamSig, e.teamSig);
}

float crosshairDistance(game::Vec2 p, float cx, float cy) {
    float dx = p.x - cx, dy = p.y - cy;
    return std::sqrt(dx * dx + dy * dy);
}

Vec3 bonePosition(const game::Entity& e, int bone) {
    switch (bone) {
        case 0: return e.headPos;
        case 1: return e.torsoPos;
        default: return e.rootPos;
    }
}

// ---- drawing helpers ----

// Thin ESP lines sit on top of a noisy 3D scene: draw a dark halo under every
// stroke so the colour reads the same over a white wall and a black tree.
void glowLine(float x1, float y1, float x2, float y2, uint32_t col, float th) {
    rnd::line(x1, y1, x2, y2, 0xB0000000, th + 2.0f);
    rnd::line(x1, y1, x2, y2, col, th);
}

void glowRect(float x, float y, float w, float h, uint32_t col, float th, float radius) {
    rnd::rectOutline(x - 1.0f, y - 1.0f, w + 2.0f, h + 2.0f, 0x8C000000, th + 1.5f, radius);
    rnd::rectOutline(x, y, w, h, col, th, radius);
}

uint32_t healthColor(float frac) {
    frac = std::clamp(frac, 0.0f, 1.0f);
    int r = static_cast<int>(255.0f * std::min(1.0f, (1.0f - frac) * 2.0f));
    int gr = static_cast<int>(255.0f * std::min(1.0f, frac * 2.0f));
    return 0xFF000000u | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(gr) << 8) | 0x30u;
}

void drawCorners(float x, float y, float w, float h, uint32_t col, float th) {
    float c = std::clamp(std::min(w, h) * 0.28f, 7.0f, 34.0f);
    glowLine(x, y, x + c, y, col, th);
    glowLine(x, y, x, y + c, col, th);
    glowLine(x + w, y, x + w - c, y, col, th);
    glowLine(x + w, y, x + w, y + c, col, th);
    glowLine(x, y + h, x + c, y + h, col, th);
    glowLine(x, y + h, x, y + h - c, col, th);
    glowLine(x + w, y + h, x + w - c, y + h, col, th);
    glowLine(x + w, y + h, x + w, y + h - c, col, th);
}

// Real skeleton: every R6/R15 joint is resolved from the character's own parts
// and the links below mirror the actual rig, instead of one head->root line.
struct BoneLink { int a, b; };

constexpr BoneLink kR15Links[] = {
    {game::BoneHead, game::BoneUpperTorso},
    {game::BoneUpperTorso, game::BoneLowerTorso},
    {game::BoneUpperTorso, game::BoneLeftUpperArm},
    {game::BoneLeftUpperArm, game::BoneLeftLowerArm},
    {game::BoneLeftLowerArm, game::BoneLeftHand},
    {game::BoneUpperTorso, game::BoneRightUpperArm},
    {game::BoneRightUpperArm, game::BoneRightLowerArm},
    {game::BoneRightLowerArm, game::BoneRightHand},
    {game::BoneLowerTorso, game::BoneLeftUpperLeg},
    {game::BoneLeftUpperLeg, game::BoneLeftLowerLeg},
    {game::BoneLeftLowerLeg, game::BoneLeftFoot},
    {game::BoneLowerTorso, game::BoneRightUpperLeg},
    {game::BoneRightUpperLeg, game::BoneRightLowerLeg},
    {game::BoneRightLowerLeg, game::BoneRightFoot},
};

void drawSkeleton(const game::Entity& e, const game::Vec2* pts, const bool* ok,
                  uint32_t col, float feetY, float thick) {
    // Each bone is extended a hair past its joints. The joints themselves are
    // never drawn, so the figure reads as continuous lines with no dots or
    // elbow/knee blobs.
    auto segment = [&](int a, int b) {
        if (!ok[a] || !ok[b]) return;
        const float dx = pts[b].x - pts[a].x;
        const float dy = pts[b].y - pts[a].y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 0.01f) return;
        const float ux = dx / len * 1.4f;
        const float uy = dy / len * 1.4f;
        glowLine(pts[a].x - ux, pts[a].y - uy, pts[b].x + ux, pts[b].y + uy, col, thick);
    };
    auto segmentTo = [&](int a, float x, float y, float ext) {
        if (!ok[a]) return;
        glowLine(pts[a].x, pts[a].y - ext, x, y, col, thick);
    };
    auto midpoint = [&](int a, int b, game::Vec2& out) {
        out.x = (pts[a].x + pts[b].x) * 0.5f;
        out.y = (pts[a].y + pts[b].y) * 0.5f;
    };

    if (e.r15) {
        for (size_t i = 0; i < sizeof(kR15Links) / sizeof(kR15Links[0]); ++i)
            segment(kR15Links[i].a, kR15Links[i].b);
    } else {
        // R6: head, torso, two arms, two legs. A plain joint-to-joint graph makes
        // an X across the chest, so draw the shoulder and hip bars explicitly and
        // hang the spine off their midpoints.
        segment(game::BoneHead, game::BoneTorso);
        segment(game::BoneLeftArm, game::BoneRightArm);
        segment(game::BoneLeftLeg, game::BoneRightLeg);
        // R6 has no separate foot parts: run the legs down to the projected
        // ground line so the figure ends at the feet instead of the knees.
        if (ok[game::BoneLeftLeg] && feetY > pts[game::BoneLeftLeg].y)
            segmentTo(game::BoneLeftLeg, pts[game::BoneLeftLeg].x, feetY, 1.4f);
        if (ok[game::BoneRightLeg] && feetY > pts[game::BoneRightLeg].y)
            segmentTo(game::BoneRightLeg, pts[game::BoneRightLeg].x, feetY, 1.4f);
        if (ok[game::BoneTorso]) {
            game::Vec2 mid{};
            if (ok[game::BoneLeftArm] && ok[game::BoneRightArm]) {
                midpoint(game::BoneLeftArm, game::BoneRightArm, mid);
                glowLine(pts[game::BoneTorso].x, pts[game::BoneTorso].y, mid.x, mid.y, col, thick);
            }
            if (ok[game::BoneLeftLeg] && ok[game::BoneRightLeg]) {
                midpoint(game::BoneLeftLeg, game::BoneRightLeg, mid);
                glowLine(pts[game::BoneTorso].x, pts[game::BoneTorso].y, mid.x, mid.y, col, thick);
            }
        }
    }
}

// Axis-aligned world box: eight projected corners, twelve edges.
void draw3DBox(const game::Camera& cam, const game::Entity& e,
               const game::Viewport& vp, uint32_t col) {
    const float hx = e.boxHalf.x, hz = e.boxHalf.z;
    const float y0 = e.boxBottom.y, y1 = e.boxTop.y;
    const float cx = e.rootPos.x, cz = e.rootPos.z;

    Vec3 corner[8] = {
        {cx - hx, y0, cz - hz}, {cx + hx, y0, cz - hz},
        {cx + hx, y0, cz + hz}, {cx - hx, y0, cz + hz},
        {cx - hx, y1, cz - hz}, {cx + hx, y1, cz - hz},
        {cx + hx, y1, cz + hz}, {cx - hx, y1, cz + hz},
    };
    static constexpr int kEdges[12][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0},
        {4, 5}, {5, 6}, {6, 7}, {7, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7},
    };

    game::Vec2 s[8]{};
    bool ok[8]{};
    for (int i = 0; i < 8; ++i) ok[i] = game::worldToScreen(cam, corner[i], vp, s[i]);

    for (const auto& edge : kEdges) {
        if (!ok[edge[0]] || !ok[edge[1]]) continue;
        glowLine(s[edge[0]].x, s[edge[0]].y, s[edge[1]].x, s[edge[1]].y, col, 1.2f);
    }
}

// Edge arrow for a target that is outside the viewport. Direction comes from
// the camera basis, so it works for targets behind the player too.
void drawOffscreenArrow(const game::Camera& cam, const game::Entity& e,
                        const game::Viewport& vp, uint32_t col) {
    const Vec3 anchor = (length(e.rootPos) > 1e-3f) ? e.rootPos : e.headPos;
    const Vec3 rel = anchor - cam.pos;
    float x = dot(rel, cam.right);
    float y = dot(rel, cam.up);
    float z = dot(rel, cam.look);
    if (z < 0.0f) { x = -x; y = -y; }   // behind: mirror through the centre
    if (std::fabs(x) < 1e-4f && std::fabs(y) < 1e-4f) return;

    const float ang = std::atan2(-y, x);
    const float c = std::cos(ang), s = std::sin(ang);
    const float cx = vp.x + vp.w * 0.5f;
    const float cy = vp.y + vp.h * 0.5f;
    const float hw = std::max(20.0f, vp.w * 0.5f - 30.0f);
    const float hh = std::max(20.0f, vp.h * 0.5f - 30.0f);
    const float tx = (std::fabs(c) > 1e-4f) ? hw / std::fabs(c) : 1.0e9f;
    const float ty = (std::fabs(s) > 1e-4f) ? hh / std::fabs(s) : 1.0e9f;
    const float t = std::min(tx, ty);

    const float px = cx + c * t;
    const float py = cy + s * t;
    const float bx = px - c * 7.0f;
    const float by = py - s * 7.0f;
    const float nx = -s * 6.0f;
    const float ny = c * 6.0f;
    glowLine(bx + nx, by + ny, px + c * 7.0f, py + s * 7.0f, col, 1.5f);
    glowLine(bx - nx, by - ny, px + c * 7.0f, py + s * 7.0f, col, 1.5f);
    glowLine(bx + nx, by + ny, bx - nx, by - ny, col, 1.5f);
}

// ---- ESP ----

int drawESP(const game::Camera& cam, const std::vector<game::Entity>& entities,
            const game::Viewport& vp, uintptr_t localTeam) {
    const uint32_t enemyCol = argbOf(g.espBoxColor);
    const uint32_t teamCol = argbOf(g.espTeamColor);
    const uint32_t mobCol = argbOf(g.espMobColor);
    const uint32_t bossCol = argbOf(g.espMobBossColor);
    int drawn = 0;

    for (const auto& e : entities) {
        const bool isMob = e.kind == game::Kind::Mob;
        if (isMob && !g.espMobs) continue;
        if (!isMob && !g.esp) continue;

        const float maxDist = isMob ? g.espMobMaxDistance : g.espMaxDistance;
        if (e.distance > maxDist) continue;

        const bool mate = !isMob && g.espTeamCheck && sameTeam(localTeam, e);
        const bool boss = isMob && e.label == "boss";
        const uint32_t col = isMob ? (boss ? bossCol : mobCol) : (mate ? teamCol : enemyCol);

        game::Vec2 headS, rootS;
        const bool haveHead = game::worldToScreen(cam, e.headPos, vp, headS);
        const bool haveRoot = game::worldToScreen(cam, e.rootPos, vp, rootS);

        const bool onScreen = haveHead &&
            headS.x >= vp.x - 4.0f && headS.x <= vp.x + vp.w + 4.0f &&
            headS.y >= vp.y - 4.0f && headS.y <= vp.y + vp.h + 4.0f;

        float fade = 1.0f;
        if (maxDist > 1.0f)
            fade = std::clamp(1.0f - (e.distance / maxDist) * 0.65f, 0.45f, 1.0f);

        if (!onScreen) {
            if (g.espOffscreen)
                drawOffscreenArrow(cam, e, vp, withAlpha(col, 0.85f * fade));
            continue;
        }
        ++drawn;

        // Project every joint once: the skeleton uses the points and the box is
        // built from their screen extents, so it hugs arms and feet instead of a
        // guessed 0.45 * height rectangle.
        game::Vec2 pts[game::BoneCount]{};
        bool ok[game::BoneCount]{};
        float minX = FLT_MAX, maxX = -FLT_MAX, minY = FLT_MAX, maxY = -FLT_MAX;
        int projected = 0;
        for (int i = 0; i < game::BoneCount; ++i) {
            if (!(e.boneMask & (1u << i))) continue;
            if (!game::worldToScreen(cam, e.bones[i], vp, pts[i])) continue;
            ok[i] = true;
            ++projected;
            minX = std::min(minX, pts[i].x);
            maxX = std::max(maxX, pts[i].x);
            minY = std::min(minY, pts[i].y);
            maxY = std::max(maxY, pts[i].y);
        }

        const uint32_t boxCol = withAlpha(col, fade);

        // Vertical extent from the real head top and the soles of the feet;
        // horizontal extent from the projected joints (arms included).
        game::Vec2 topS, botS;
        float y0, y1;
        const bool haveV = game::worldToScreen(cam, e.boxTop, vp, topS) &&
                           game::worldToScreen(cam, e.boxBottom, vp, botS) &&
                           botS.y - topS.y > 8.0f;
        if (haveV) {
            y0 = topS.y;
            y1 = botS.y;
        } else if (projected >= 3 && maxY - minY > 8.0f) {
            y0 = minY;
            y1 = maxY;
        } else {
            y0 = headS.y - 10.0f;
            y1 = haveRoot ? rootS.y + std::fabs(headS.y - rootS.y) : headS.y + 60.0f;
        }
        float x0, x1;
        if (projected >= 3 && maxX - minX > 4.0f) {
            x0 = minX;
            x1 = maxX;
        } else {
            const float h = (y1 - y0) * 0.45f;
            x0 = headS.x - h * 0.5f;
            x1 = headS.x + h * 0.5f;
        }

        // Joints collapse onto each other in profile view, so keep a sane aspect
        // instead of drawing a sliver around the torso.
        const float minW = (y1 - y0) * 0.34f;
        if (x1 - x0 < minW) {
            const float mid = (x0 + x1) * 0.5f;
            x0 = mid - minW * 0.5f;
            x1 = mid + minW * 0.5f;
        }

        const float pad = 3.0f;
        const float bx = std::floor(x0 - pad);
        const float by = std::floor(y0 - pad);
        const float boxW = std::floor(x1 - x0 + pad * 2.0f);
        const float boxH = std::floor(y1 - y0 + pad * 2.0f);

        // ---- box ----
        if (g.espFill)
            rnd::rect(bx, by, boxW, boxH, withAlpha(col, 0.055f), 3.0f);

        const bool wantBox = isMob ? g.espMobBox : g.espBox;
        if (wantBox) {
            const int style = isMob ? g.espMobStyle : g.espStyle;
            switch (style) {
                case 1: drawCorners(bx, by, boxW, boxH, boxCol, mate ? 1.2f : 1.7f); break;
                case 2: draw3DBox(cam, e, vp, boxCol); break;
                default: glowRect(bx, by, boxW, boxH, boxCol, mate ? 1.0f : 1.4f, 2.0f); break;
            }
        }

        const bool wantSkeleton = isMob ? g.espMobSkeleton : g.espSkeleton;
        if (wantSkeleton)
            drawSkeleton(e, pts, ok, boxCol, y1, std::clamp(g.espSkeletonThickness, 0.8f, 4.0f));

        // ---- head marker ----
        if (g.espHeadDot && wantBox && ok[game::BoneHead]) {
            float r = std::clamp(boxH * 0.055f, 2.0f, 5.0f);
            rnd::circle(pts[game::BoneHead].x, pts[game::BoneHead].y, r,
                        withAlpha(col, 0.9f * fade), 1.2f);
        }

        // ---- health bar ----
        const bool wantHealth = isMob ? g.espMobHealth : g.espHealth;
        if (wantHealth && e.humanoidAddr) {
            float frac = (e.maxHealth > 0.0f) ? (e.health / e.maxHealth) : 0.0f;
            frac = std::clamp(frac, 0.0f, 1.0f);
            const float barW = std::max(2.0f, std::floor(boxW * 0.055f));
            const bool right = g.espHealthSide == 1;
            const float hx = right ? std::floor(bx + boxW + 4.0f)
                                   : std::floor(bx - barW - 4.0f);
            rnd::rect(hx - 1.0f, by - 1.0f, barW + 2.0f, boxH + 2.0f, 0x99000000, 1.0f);

            if (frac > 0.0f) {
                if (g.espHealthGradient) {
                    const uint32_t bottom = healthColor(frac);
                    rnd::rectGradient(hx, by + boxH * (1.0f - frac), barW, boxH * frac,
                                      lighten(bottom, 0.35f), bottom);
                } else {
                    rnd::rect(hx, by + boxH * (1.0f - frac), barW, boxH * frac,
                              healthColor(frac), 0.0f);
                }
            }

            if (g.espHealthText || g.espMaxHealth || (isMob && g.espMobHealth)) {
                char hp[32];
                if (g.espMaxHealth && e.maxHealth > 0.0f)
                    std::snprintf(hp, sizeof(hp), "%d/%d", static_cast<int>(e.health + 0.5f),
                                  static_cast<int>(e.maxHealth + 0.5f));
                else
                    std::snprintf(hp, sizeof(hp), "%d", static_cast<int>(e.health + 0.5f));
                const float tw = rnd::textWidth(hp, 10.0f);
                const float tx = right ? hx + barW + 4.0f : hx - tw - 2.0f;
                textShadow(std::floor(tx), std::floor(by), hp,
                           withAlpha(0xFFE6E6EA, fade), 10.0f);
            }
        }

        // ---- name / mob label ----
        const bool wantName = isMob ? (g.espMobName || g.espMobLabels) : g.espName;
        std::string display = e.name;
        if (isMob && g.espMobLabels && !e.label.empty()) {
            if (display.empty()) display = "[" + e.label + "]";
            else display = "[" + e.label + "] " + display;
        }
        if (wantName && !display.empty()) {
            const float fs = std::clamp(boxH * 0.14f, 10.0f, 14.0f);
            const float th = rnd::textHeight(fs);
            float tw = rnd::textWidth(display.c_str(), fs, true);
            const uint32_t nameCol = withAlpha(0xFFF2F4F8, fade);

            if (g.espNameBg) {
                const float maxTw = std::clamp(boxW * 1.7f, 78.0f, 200.0f);
                const bool clipped = tw > maxTw;
                if (clipped) tw = maxTw;

                const float pillH = std::floor(th + 5.0f);
                const float pillW = std::floor(tw + 20.0f);
                const float px = std::clamp(std::floor(headS.x - pillW * 0.5f),
                                            vp.x + 2.0f, vp.x + vp.w - pillW - 2.0f);
                const float py = std::floor(std::max(vp.y + 2.0f, by - pillH - 6.0f));
                const float tx = px + 13.0f;
                const float ty = py + std::floor((pillH - th) * 0.5f);

                rnd::rect(px, py, pillW, pillH, withAlpha(0xDA0A0B0F, fade), pillH * 0.5f);
                rnd::rectOutline(px + 0.5f, py + 0.5f, pillW - 1.0f, pillH - 1.0f,
                                 withAlpha(col, 0.38f * fade), 1.0f, pillH * 0.5f);
                rnd::rect(px + 5.5f, py + 3.5f, 2.0f, pillH - 7.0f,
                          withAlpha(col, 0.95f * fade), 1.0f);

                if (clipped)
                    rnd::textClipped(tx, ty, tw, display.c_str(), nameCol, fs, true);
                else
                    rnd::text(tx, ty, display.c_str(), nameCol, fs, true);
            } else {
                const float nx = std::clamp(std::floor(headS.x - tw * 0.5f),
                                            vp.x + 2.0f, vp.x + vp.w - tw - 2.0f);
                const float ny = std::floor(std::max(vp.y + 2.0f, by - th - 5.0f));
                textOutlined(nx, ny, display.c_str(), nameCol, fs, true);
            }
        }

        // ---- weapon / tool ----
        if (!isMob && g.espWeapon && !e.toolName.empty()) {
            char tool[64];
            std::snprintf(tool, sizeof(tool), "%s", e.toolName.c_str());
            const float tw = rnd::textWidth(tool, 9.5f);
            const float tx = std::clamp(std::floor(headS.x - tw * 0.5f),
                                        vp.x + 2.0f, vp.x + vp.w - tw - 2.0f);
            const float ty = std::floor(std::max(vp.y + 2.0f, by - rnd::textHeight(9.5f) - 20.0f));
            textShadow(tx, ty, tool, withAlpha(0xFFAEB6C2, fade), 9.5f);
        }

        // ---- distance ----
        const bool wantDist = isMob ? g.espMobDistance : g.espDistance;
        if (wantDist) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%dm", static_cast<int>(e.distance));
            const float tw = rnd::textWidth(buf, 10.5f);
            textShadow(std::floor(bx + boxW * 0.5f - tw * 0.5f),
                       std::floor(by + boxH + 4.0f),
                       buf, withAlpha(0xFFC6CDD8, fade), 10.5f);
        }

        // ---- tracer ----
        if (!isMob && g.espTracer) {
            glowLine(vp.x + vp.w * 0.5f, vp.y + vp.h, headS.x, by + boxH * 0.5f,
                     withAlpha(col, 0.55f * fade), 1.0f);
        }
    }
    return drawn;
}

// ---- Aimbot ----

uintptr_t g_localPlayer = 0;  // set by update(), used by the visibility test
uintptr_t g_aimTarget = 0;    // sticky target key (Player address or character)

// "Visible only": every other player/mob is treated as two spheres (torso +
// head) and the lock is rejected when someone else sits closer to the camera
// along the ray to the aim point. Walls and map geometry are handled by the
// cached occluder set in world.cpp.
bool aimClear(const Vec3& origin, const Vec3& point,
              const std::vector<game::Entity>& entities,
              uintptr_t targetKey, uintptr_t self) {
    if (world::segmentBlocked(origin, point)) return false;

    const Vec3 seg = point - origin;
    const float dist = length(seg);
    if (dist < 1e-3f) return true;
    const Vec3 dir = seg * (1.0f / dist);

    for (const auto& o : entities) {
        if (o.ghost) continue;   // streamed-out body: not at a live position
        if (o.key() == targetKey) continue;
        if (self && o.playerAddr == self) continue;

        const Vec3 centers[2] = {o.torsoPos, o.headPos};
        // Tight radii: a generous sphere rejects every ray that passes a stud
        // or two beside a player, which made the lock drop constantly in a
        // crowded server.
        const float radii[2] = {1.05f, 0.6f};
        for (int i = 0; i < 2; ++i) {
            if (length(centers[i]) < 1e-3f) continue;
            const Vec3 oc = centers[i] - origin;
            const float t = dot(oc, dir);
            if (t <= 0.0f || t >= dist) continue;
            const float perp2 = dot(oc, oc) - t * t;
            if (perp2 < radii[i] * radii[i]) return false;
        }
    }
    return true;
}

void runAimbot(const game::Camera& cam, const std::vector<game::Entity>& entities,
               const game::Viewport& vp, uintptr_t localTeam, bool active) {
    static auto lastTick = std::chrono::steady_clock::now();
    static bool clockValid = false;
    static uintptr_t lastLocked = 0;
    static auto lastTrigger = std::chrono::steady_clock::now();

    if (!active) {
        g_aimTarget = 0;
        lastLocked = 0;
        clockValid = false;
        return;
    }

    // Real frame time, so smoothing covers the same distance per second on a
    // 60 Hz and a 144 Hz display instead of a fixed slice per frame.
    const auto tick = std::chrono::steady_clock::now();
    float dt = clockValid ? std::chrono::duration<float>(tick - lastTick).count()
                          : (1.0f / 60.0f);
    lastTick = tick;
    clockValid = true;
    dt = std::clamp(dt, 0.002f, 0.1f);

    const float cx = vp.x + vp.w * 0.5f;
    const float cy = vp.y + vp.h * 0.5f;
    const Vec3 origin = cam.pos;
    const Vec3 forward = normalize(cam.look);

    // A candidate is only "hidden" when the visibility test - and nothing else
    // - rejected it; that distinction is what lets the lock ride out a short
    // occlusion instead of being thrown away.
    enum Lock { Locked, Hidden, Rejected };

    auto evaluate = [&](const game::Entity& e, Vec3& outPos, float& outScore) -> Lock {
        if (e.ghost) return Rejected;   // streamed-out: position is not live
        if (e.kind == game::Kind::Mob && !g.aimMobs) return Rejected;
        if (e.distance > g.aimMaxDistance) return Rejected;
        if (e.kind == game::Kind::Player && g.aimTeamCheck && sameTeam(localTeam, e))
            return Rejected;

        // Aim points in priority order: the chosen bone first, then the rest of
        // the body. Someone half behind a prop or a low wall then locks on the
        // part that is actually visible instead of being skipped entirely.
        const Vec3 points[3] = {bonePosition(e, g.aimBone), e.headPos, e.torsoPos};
        bool anyHidden = false;

        for (int i = 0; i < 3; ++i) {
            Vec3 target = points[i];
            if (length(target) < 1e-3f) continue;

            bool duplicate = false;
            for (int j = 0; j < i; ++j)
                if (length(points[j] - target) < 0.05f) duplicate = true;
            if (duplicate) continue;

            if (g.aimPrediction > 0.0f) {
                auto it = g_prevPos.find(e.key());
                if (it != g_prevPos.end()) {
                    Vec3 delta = target - it->second;
                    if (length(delta) < 100.0f) target = target + delta * g.aimPrediction;
                }
            }

            if (angleDeg(forward, target - origin) > g.aimFov * 0.5f) continue;

            game::Vec2 sp;
            if (!game::worldToScreen(cam, target, vp, sp)) continue;

            if (g.aimVisibleOnly &&
                !aimClear(origin, target, entities, e.key(), g_localPlayer)) {
                anyHidden = true;
                continue;
            }

            outPos = target;
            switch (g.aimPriority) {
                case 1: outScore = e.distance; break;          // nearest body
                case 2: outScore = std::max(0.0f, e.health); break; // lowest hp
                default: outScore = crosshairDistance(sp, cx, cy); break;
            }
            return Locked;
        }

        return anyHidden ? Hidden : Rejected;
    };

    const game::Entity* best = nullptr;
    Vec3 bestPos{};

    // Sticky: once locked, stay on that target while it stays valid - stops the
    // crosshair dancing between two players. A hidden target does not block the
    // scan below, so the moment anyone comes into view they are locked without
    // having to re-press the key.
    if (g.aimSticky && g_aimTarget) {
        for (const auto& e : entities) {
            if (e.key() != g_aimTarget) continue;
            Vec3 pos{};
            float score = 0.0f;
            if (evaluate(e, pos, score) == Locked) {
                best = &e;
                bestPos = pos;
            }
            break;
        }
    }

    if (!best) {
        float bestScore = FLT_MAX;
        for (const auto& e : entities) {
            Vec3 pos{};
            float score = 0.0f;
            if (evaluate(e, pos, score) != Locked) continue;
            if (score < bestScore) {
                bestScore = score;
                best = &e;
                bestPos = pos;
            }
        }
    }

    g_prevPos.clear();
    for (const auto& e : entities) {
        Vec3 p = bonePosition(e, g.aimBone);
        if (length(p) > 1e-3f) g_prevPos[e.key()] = p;
    }

    if (!best) {
        g_aimTarget = 0;
        return;
    }
    g_aimTarget = best->key();

    game::Vec2 sp;
    if (!game::worldToScreen(cam, bestPos, vp, sp)) return;

    // Triggerbot: fire when the target sits inside the trigger radius. It runs
    // off the real target position, not the smoothed cursor, so it still fires
    // while the aim step is easing in.
    if (g.aimTriggerbot && !overlay::menuOpen()) {
        const float d = crosshairDistance(sp, cx, cy);
        if (d <= std::max(2.0f, g.aimTriggerRadius)) {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastTrigger > std::chrono::milliseconds(40)) {
                sendClick();
                lastTrigger = now;
            }
        }
    }

    const float smooth = std::max(1.0f, g.aimSmooth);
    const float base = 1.0f / smooth;
    float step = 1.0f - std::pow(1.0f - base, dt * 60.0f);

    // First frame on a new target takes a bigger bite, so the aim visibly
    // starts the moment the enemy appears instead of easing in over a few
    // frames. Tracking after that frame is untouched.
    if (best->key() != lastLocked) step = std::max(step, 0.5f);
    lastLocked = best->key();

    float dx = (sp.x - cx) * step;
    float dy = (sp.y - cy) * step;

    // Humanization: a little random offset on the final step. Keeps a
    // pixel-perfect snap from looking like a machine.
    if (g.aimHumanize > 0.01f) {
        static std::mt19937 rng{std::random_device{}()};
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        dx += dist(rng) * g.aimHumanize;
        dy += dist(rng) * g.aimHumanize;
    }

    sendMouseMove(dx, dy);
}

// ---- memory features ----

// Originals captured before the first write, restored on exit. Keeping them at
// file scope (instead of inside applyVisuals) lets restoreVisuals put back the
// exact values the place shipped with, not a guessed default.
bool  g_lightSaved = false;
float g_origBrightness = 1.0f;
float g_origFogEnd = 100000.0f;
bool  g_fovSaved = false;
float g_origFovDeg = 70.0f;

void applyVisuals() {
    rb::Instance lighting = rb::lighting();
    if (lighting) {
        if (!g_lightSaved) {
            g_origBrightness = mem::g.read<float>(lighting.address() + off::LightingBrightness);
            g_origFogEnd = mem::g.read<float>(lighting.address() + off::LightingFogEnd);
            if (!(g_origBrightness > -100.0f && g_origBrightness < 100.0f)) g_origBrightness = 1.0f;
            if (!(g_origFogEnd > 0.0f && g_origFogEnd < 1.0e9f)) g_origFogEnd = 100000.0f;
            g_lightSaved = true;
        }
        if (g.fullbright)
            mem::g.write<float>(lighting.address() + off::LightingBrightness, g.fullbrightValue);
        if (g.noFog)
            mem::g.write<float>(lighting.address() + off::LightingFogEnd, 1.0e7f);
    }

    if (g.fovChanger) {
        rb::Instance cam = rb::camera();
        if (cam) {
            // Camera.FieldOfView is stored in radians. Only write when it drifts,
            // so the game's own camera script is not fought every frame.
            if (!g_fovSaved) {
                const float cur = mem::g.read<float>(cam.address() + off::CameraFOV);
                const float deg = cur * (180.0f / 3.14159265358979323846f);
                if (deg > 1.0f && deg < 179.0f) g_origFovDeg = deg;
                g_fovSaved = true;
            }
            float want = g.fovValue * (3.14159265358979323846f / 180.0f);
            float cur = mem::g.read<float>(cam.address() + off::CameraFOV);
            if (std::fabs(cur - want) > 0.01f)
                mem::g.write<float>(cam.address() + off::CameraFOV, want);
        }
    }
}

// Put back the values we overwrote, so closing the overlay does not leave the
// game stuck at 120 degrees FOV or brightness 3 until the next rejoin.
void restoreVisuals() {
    rb::Instance lighting = rb::lighting();
    if (lighting) {
        if (g.fullbright)
            mem::g.write<float>(lighting.address() + off::LightingBrightness, g_origBrightness);
        if (g.noFog)
            mem::g.write<float>(lighting.address() + off::LightingFogEnd, g_origFogEnd);
    }
    if (g.fovChanger) {
        rb::Instance cam = rb::camera();
        if (cam)
            mem::g.write<float>(cam.address() + off::CameraFOV,
                                g_origFovDeg * (3.14159265358979323846f / 180.0f));
    }
}

void applyMovement() {
    if (!g.walkSpeed && !g.jumpPower) return;

    rb::Instance lp = rb::localPlayer();
    if (!lp) return;
    uintptr_t character = mem::g.read<uintptr_t>(lp.address() + off::PlayerModel);
    if (!character) return;

    // single child pass instead of a class-name walk every frame
    uintptr_t humanoid = 0;
    for (const auto& c : rb::Instance(character).children()) {
        if (c.name() == "Humanoid") { humanoid = c.address(); break; }
    }
    if (!humanoid) return;

    if (g.walkSpeed)
        mem::g.write<float>(humanoid + off::HumanoidWalkSpeed, g.walkSpeedValue);
    if (g.jumpPower)
        mem::g.write<float>(humanoid + off::HumanoidJumpPower, g.jumpPowerValue);
}

// ---- utility ----

uintptr_t localRootPart() {
    rb::Instance lp = rb::localPlayer();
    if (!lp) return 0;
    uintptr_t character = mem::g.read<uintptr_t>(lp.address() + off::PlayerModel);
    if (!character) return 0;
    return rb::Instance(character).findFirstChild("HumanoidRootPart").address();
}

bool teleportLocal(const Vec3& p) {
    const uintptr_t root = localRootPart();
    if (!root) return false;
    const uintptr_t prim = mem::g.read<uintptr_t>(root + off::PartPrimitive);
    if (!prim) return false;
    return mem::g.writeRaw(prim + off::PrimitivePosition, &p, sizeof(p));
}

bool readLocalPosition(Vec3& out) {
    const uintptr_t root = localRootPart();
    if (!root) return false;
    out = game::readPartPosition(root);
    return length(out) > 1e-3f;
}

void applyUtility() {
    // Anti-AFK: a one-pixel mouse nudge every minute. Roblox's idle counter
    // only looks for input, so this keeps the session alive without pressing a
    // gameplay key.
    static ULONGLONG lastAfk = 0;
    if (g.antiAfk) {
        const ULONGLONG now = GetTickCount64();
        if (!lastAfk) lastAfk = now;
        if (now - lastAfk > 60000) {
            lastAfk = now;
            INPUT in{};
            in.type = INPUT_MOUSE;
            in.mi.dx = 1;
            in.mi.dwFlags = MOUSEEVENTF_MOVE;
            SendInput(1, &in, sizeof(in));
            in.mi.dx = -1;
            SendInput(1, &in, sizeof(in));
        }
    } else {
        lastAfk = 0;
    }

    // Auto clicker: hold the bound key to click at the configured rate. It is
    // disabled while the menu is open so it cannot fight the panel.
    if (g.autoClick && !overlay::menuOpen() && keyDown(g.autoClickKey)) {
        static ULONGLONG lastClick = 0;
        const float cps = std::clamp(g.autoClickCps, 1.0f, 25.0f);
        const ULONGLONG interval = static_cast<ULONGLONG>(1000.0f / cps);
        const ULONGLONG now = GetTickCount64();
        if (now - lastClick >= interval) {
            lastClick = now;
            sendClick();
        }
    }
}

// ---- menu ----

const char* const kTabs[] = {"ESP", "Mobs", "Aimbot", "Visuals", "Player", "Config"};
int g_tab = 0;

void drawMenu() {
    ui::panelBegin("ELYYYKKK", 64.0f, 40.0f, 700.0f, 440.0f);
    ui::tabBar(&g_tab, kTabs, 6, g_tab);

    switch (g_tab) {
        case 0: {
            static const char* const styles[] = {"box", "corners", "3d box"};
            static const char* const sides[] = {"left", "right"};
            ui::columns(2);

            ui::section("players");
            ui::checkbox(&g.esp, "enabled", g.esp);
            ui::checkbox(&g.espBox, "box", g.espBox);
            ui::cycle(&g.espStyle, "style", g.espStyle, styles, 3);
            ui::checkbox(&g.espFill, "fill", g.espFill);
            ui::checkbox(&g.espSkeleton, "skeleton", g.espSkeleton);
            if (g.espSkeleton)
                ui::slider(&g.espSkeletonThickness, "bone thickness",
                           g.espSkeletonThickness, 0.8f, 4.0f, "%.1f");
            ui::checkbox(&g.espName, "names", g.espName);
            ui::checkbox(&g.espWeapon, "weapon", g.espWeapon);
            ui::checkbox(&g.espHealth, "health", g.espHealth);
            ui::checkbox(&g.espMaxHealth, "hp / max", g.espMaxHealth);
            ui::checkbox(&g.espHealthGradient, "health gradient", g.espHealthGradient);
            ui::cycle(&g.espHealthSide, "health side", g.espHealthSide, sides, 2);
            ui::checkbox(&g.espDistance, "distance", g.espDistance);

            ui::nextColumn();
            ui::section("filter");
            ui::checkbox(&g.espTracer, "tracer", g.espTracer);
            ui::checkbox(&g.espOffscreen, "off-screen arrows", g.espOffscreen);
            ui::checkbox(&g.espList, "player list", g.espList);
            ui::checkbox(&g.espTeamCheck, "team check", g.espTeamCheck);
            ui::checkbox(&g.espSelf, "draw self (test)", g.espSelf);
            ui::slider(&g.espMaxDistance, "max distance", g.espMaxDistance, 50.0f, 5000.0f, "%.0f");

            ui::section("colors");
            ui::swatches(&g.espBoxColor, g.espBoxColor, "enemy");
            ui::swatches(&g.espTeamColor, g.espTeamColor, "team");
            break;
        }

        case 1: {
            static const char* const styles[] = {"box", "corners", "3d box"};
            ui::columns(2);
            ui::section("mobs / npcs");
            ui::checkbox(&g.espMobs, "enabled", g.espMobs);
            ui::checkbox(&g.espMobBox, "box", g.espMobBox);
            ui::cycle(&g.espMobStyle, "style", g.espMobStyle, styles, 3);
            ui::checkbox(&g.espMobSkeleton, "skeleton", g.espMobSkeleton);
            ui::checkbox(&g.espMobName, "name", g.espMobName);
            ui::checkbox(&g.espMobLabels, "type tag", g.espMobLabels);
            ui::checkbox(&g.espMobHealth, "health", g.espMobHealth);
            ui::checkbox(&g.espMobDistance, "distance", g.espMobDistance);

            ui::nextColumn();
            ui::section("filter");
            ui::slider(&g.espMobMaxDistance, "max distance", g.espMobMaxDistance,
                       50.0f, 5000.0f, "%.0f");
            ui::checkbox(&g.aimMobs, "aimbot targets mobs", g.aimMobs);
            {
                char info[48];
                std::snprintf(info, sizeof(info), "%d tracked", static_cast<int>(g_mobs.size()));
                ui::dim(info);
            }

            ui::section("colors");
            ui::swatches(&g.espMobColor, g.espMobColor, "mob");
            ui::swatches(&g.espMobBossColor, g.espMobBossColor, "boss");
            break;
        }

        case 2: {
            static const char* const bones[] = {"head", "torso", "root"};
            static const char* const modes[] = {"hold", "toggle"};
            static const char* const prio[] = {"crosshair", "distance", "lowest hp"};

            ui::columns(2);
            ui::section("target");
            ui::checkbox(&g.aim, "enabled", g.aim);
            ui::checkbox(&g.aimTeamCheck, "team check", g.aimTeamCheck);
            ui::checkbox(&g.aimMobs, "target mobs", g.aimMobs);
            ui::checkbox(&g.aimVisibleOnly, "visible only", g.aimVisibleOnly);
            if (g.aimVisibleOnly) {
                char cache[64];
                if (!world::ready())
                    std::snprintf(cache, sizeof(cache), "mapping walls...");
                else
                    std::snprintf(cache, sizeof(cache), "%zu wall parts cached",
                                  world::occluders());
                ui::dim(cache);
            }
            ui::cycle(&g.aimBone, "bone", g.aimBone, bones, 3);
            ui::cycle(&g.aimPriority, "priority", g.aimPriority, prio, 3);
            ui::cycle(&g.aimMode, "trigger", g.aimMode, modes, 2);
            ui::keybind(&g.aimKey, "aim key", g.aimKey);
            ui::checkbox(&g.aimSticky, "sticky target", g.aimSticky);
            ui::checkbox(&g.aimFovCircle, "fov circle", g.aimFovCircle);

            ui::nextColumn();
            ui::section("behaviour");
            ui::slider(&g.aimFov, "fov", g.aimFov, 5.0f, 360.0f, "%.0f deg");
            ui::slider(&g.aimSmooth, "smooth", g.aimSmooth, 1.0f, 40.0f, "%.1f");
            ui::slider(&g.aimMaxDistance, "max distance", g.aimMaxDistance, 50.0f, 3000.0f, "%.0f");
            ui::slider(&g.aimPrediction, "prediction", g.aimPrediction, 0.0f, 4.0f, "%.2f");
            ui::slider(&g.aimHumanize, "humanize", g.aimHumanize, 0.0f, 6.0f, "%.1f px");

            ui::section("triggerbot");
            ui::checkbox(&g.aimTriggerbot, "auto fire", g.aimTriggerbot);
            if (g.aimTriggerbot)
                ui::slider(&g.aimTriggerRadius, "radius", g.aimTriggerRadius, 2.0f, 60.0f, "%.0f px");

            ui::section("fov color");
            ui::swatches(&g.aimFovColor, g.aimFovColor, "ring");
            break;
        }

        case 3:
            ui::columns(2);
            ui::section("world");
            ui::checkbox(&g.fullbright, "fullbright", g.fullbright);
            if (g.fullbright)
                ui::slider(&g.fullbrightValue, "brightness", g.fullbrightValue, 0.0f, 10.0f, "%.1f");
            ui::checkbox(&g.noFog, "no fog", g.noFog);

            ui::section("camera");
            ui::checkbox(&g.fovChanger, "fov changer", g.fovChanger);
            if (g.fovChanger)
                ui::slider(&g.fovValue, "field of view", g.fovValue, 30.0f, 120.0f, "%.0f deg");
            ui::checkbox(&g.crosshair, "crosshair", g.crosshair);
            if (g.crosshair)
                ui::slider(&g.crosshairSize, "crosshair size", g.crosshairSize, 3.0f, 24.0f, "%.0f");

            ui::nextColumn();
            ui::section("hud");
            ui::checkbox(&g.watermark, "watermark", g.watermark);
            ui::checkbox(&g.fpsCounter, "fps counter", g.fpsCounter);
            break;

        case 4: {
            static int g_scanId = 0;
            static int wpSaveId[3] = {0, 0, 0};
            static int wpTpId[3] = {0, 0, 0};

            ui::columns(2);
            ui::section("movement");
            ui::checkbox(&g.walkSpeed, "walk speed", g.walkSpeed);
            if (g.walkSpeed)
                ui::slider(&g.walkSpeedValue, "speed", g.walkSpeedValue, 16.0f, 500.0f, "%.0f");
            ui::checkbox(&g.jumpPower, "jump power", g.jumpPower);
            if (g.jumpPower)
                ui::slider(&g.jumpPowerValue, "jump", g.jumpPowerValue, 50.0f, 500.0f, "%.0f");

            ui::section("utility");
            ui::checkbox(&g.antiAfk, "anti-afk", g.antiAfk);
            ui::checkbox(&g.autoClick, "auto clicker", g.autoClick);
            if (g.autoClick) {
                ui::keybind(&g.autoClickKey, "click key", g.autoClickKey);
                ui::slider(&g.autoClickCps, "clicks / sec", g.autoClickCps, 1.0f, 25.0f, "%.0f");
            }

            static int tpEnemyId = 0;
            if (ui::button(&tpEnemyId, "teleport to nearest enemy", 200.0f)) {
                const game::Entity* best = nullptr;
                float bestDist = FLT_MAX;
                for (const auto& e : g_entities) {
                    if (e.ghost || e.kind != game::Kind::Player) continue;
                    if (g.espTeamCheck && sameTeam(g_localTeam, e)) continue;
                    if (e.distance < bestDist) {
                        bestDist = e.distance;
                        best = &e;
                    }
                }
                if (!best) {
                    toast("no enemy found");
                } else {
                    Vec3 dest = best->rootPos;
                    Vec3 local{};
                    if (readLocalPosition(local)) {
                        const Vec3 dir = normalize(local - dest);
                        dest = dest + dir * 3.0f;
                    }
                    toast(teleportLocal(dest) ? "teleport written" : "teleport failed");
                }
            }

            ui::nextColumn();
            ui::section("waypoints");
            for (int i = 0; i < 3; ++i) {
                char row[32];
                std::snprintf(row, sizeof(row), "slot %d", i + 1);
                ui::label(row);

                if (ui::button(&wpSaveId[i], "save current", 130.0f)) {
                    Vec3 p{};
                    if (readLocalPosition(p)) {
                        g.waypoint[i][0] = p.x;
                        g.waypoint[i][1] = p.y;
                        g.waypoint[i][2] = p.z;
                        g.waypointSet[i] = true;
                        toast("waypoint saved");
                    } else {
                        toast("no local character");
                    }
                }
                if (ui::button(&wpTpId[i], "teleport", 130.0f)) {
                    if (!g.waypointSet[i]) {
                        toast("slot is empty");
                    } else {
                        Vec3 p{g.waypoint[i][0], g.waypoint[i][1], g.waypoint[i][2]};
                        toast(teleportLocal(p) ? "teleport written" : "teleport failed");
                    }
                }
            }

            ui::section("maintenance");
            if (ui::button(&g_scanId, "rescan offsets"))
                rb::forceRescan();
            ui::dim("external writes. server-side");
            ui::dim("checks still see them.");
            break;
        }

        case 5: {
            static int saveId = 0, loadId = 0, resetId = 0;

            ui::columns(2);
            ui::section("settings");
            if (ui::button(&saveId, "save settings")) {
                toast(saveConfig() ? "saved config.ini" : "could not write config.ini");
            }
            if (ui::button(&loadId, "load settings")) {
                toast(loadConfig() ? "loaded config.ini" : "could not read config.ini");
            }
            if (ui::button(&resetId, "reset to defaults")) {
                resetDefaults();
                toast("defaults restored");
            }

            ui::nextColumn();
            ui::section("about");
            ui::label("ELYYYKKK");
            ui::dim("external overlay");
            ui::dim("config.ini sits beside the exe");
            ui::dim("settings save on exit");
            ui::dim("menu key: F1");
            break;
        }
    }

    if (GetTickCount64() < g_toastUntil)
        ui::sideStatus(g_toast, 0xFF5E9EFF);
    else if (!mem::g.valid())
        ui::sideStatus("not attached", 0xFF8A9099);
    else if (!rb::dataModel())
        ui::sideStatus("stale offsets", 0xFFFF5A5A);
    else
        ui::sideStatus("live", 0xFF32D74B);

    ui::panelEnd();
}

void drawOverlays(const game::Viewport& vp, float sw) {
    constexpr uint32_t kHudBg   = 0xF00E0F12;
    constexpr uint32_t kHudEdge = 0xFF25282E;
    constexpr uint32_t kHudText = 0xFFE7E9ED;
    constexpr uint32_t kHudSub  = 0xFFA0A6B1;
    constexpr uint32_t kHudFaint = 0xFF6C7280;
    constexpr uint32_t kAccent  = 0xFF5E9EFF;

    if (g.watermark) {
        const float x = 12.0f, y = 12.0f, h = 28.0f;
        char count[48];
        std::snprintf(count, sizeof(count), "%d players  %d mobs",
                      static_cast<int>(g_entities.size()),
                      static_cast<int>(g_mobs.size()));

        const float tw = rnd::textWidth("ELYYYKKK", 12.5f, true);
        const float cw = rnd::textWidth(count, 11.0f);
        const float w = 21.0f + tw + 10.0f + cw + 13.0f;

        rnd::rect(x, y, w, h, kHudBg, 7.0f);
        rnd::rectOutline(x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f, kHudEdge, 1.0f, 7.0f);
        rnd::rect(x + 9.0f, y + 8.0f, 3.0f, 12.0f, kAccent, 1.5f);
        rnd::text(x + 19.0f, y + 7.5f, "ELYYYKKK", kHudText, 12.5f, true);
        rnd::text(x + 19.0f + tw + 10.0f, y + 9.0f, count, kHudFaint, 11.0f);
    }

    if (g.fpsCounter) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f fps", g_fps);
        float tw = rnd::textWidth(buf, 11.5f);
        float w = tw + 18.0f;
        rnd::rect(sw - w - 12.0f, 12.0f, w, 24.0f, kHudBg, 6.0f);
        rnd::rectOutline(sw - w - 11.5f, 12.5f, w - 1.0f, 23.0f, kHudEdge, 1.0f, 6.0f);
        rnd::text(sw - w - 12.0f + 9.0f, 17.5f, buf, kHudSub, 11.5f);
    }

    // ---- player list ----
    if (g.espList && !g_all.empty()) {
        std::vector<const game::Entity*> list;
        list.reserve(g_all.size());
        for (const auto& e : g_all) {
            if (e.kind == game::Kind::Mob && !g.espMobs) continue;
            const float maxDist = e.kind == game::Kind::Mob ? g.espMobMaxDistance : g.espMaxDistance;
            if (e.distance <= maxDist) list.push_back(&e);
        }
        std::sort(list.begin(), list.end(),
                  [](const game::Entity* a, const game::Entity* b) {
                      return a->distance < b->distance;
                  });
        if (list.size() > 14) list.resize(14);

        const float x = 12.0f;
        float y = g.watermark ? 48.0f : 12.0f;
        const float rowH = 18.0f;
        const float w = 230.0f;
        const float h = rowH * static_cast<float>(list.size()) + 40.0f;

        rnd::rect(x, y, w, h, kHudBg, 8.0f);
        rnd::rectOutline(x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f, kHudEdge, 1.0f, 8.0f);
        rnd::rect(x + 12.0f, y + 0.5f, 30.0f, 2.0f, kAccent, 1.0f);

        rnd::text(x + 13.0f, y + 10.0f, "ENTITIES", kHudFaint, 10.0f, true);
        char nbuf[24];
        std::snprintf(nbuf, sizeof(nbuf), "%d", static_cast<int>(list.size()));
        float nw = rnd::textWidth(nbuf, 10.0f) + 12.0f;
        rnd::rect(x + w - 13.0f - nw, y + 8.0f, nw, 15.0f, 0x14FFFFFF, 4.0f);
        rnd::text(x + w - 13.0f - nw + 6.0f, y + 10.0f, nbuf, kHudSub, 10.0f);

        y += 28.0f;
        for (const auto* e : list) {
            const bool isMob = e->kind == game::Kind::Mob;
            char hp[24];
            std::snprintf(hp, sizeof(hp), "%d", static_cast<int>(e->health + 0.5f));
            float frac = (e->maxHealth > 0.0f)
                             ? std::clamp(e->health / e->maxHealth, 0.0f, 1.0f)
                             : 0.0f;

            const float* mobColRef = (e->label == "boss") ? g.espMobBossColor
                                                          : g.espMobColor;
            const uint32_t dot = isMob ? argbOf(mobColRef) : healthColor(frac);
            rnd::rect(x + 10.0f, y + 5.0f, 2.0f, 9.0f, dot, 1.0f);

            std::string name = e->name;
            if (isMob && g.espMobLabels && !e->label.empty()) {
                if (name.empty()) name = "[" + e->label + "]";
                else name = "[" + e->label + "] " + name;
            }
            rnd::textClipped(x + 18.0f, y + 1.0f, 132.0f, name.c_str(), kHudText, 11.5f);

            if (!isMob)
                rnd::text(x + 154.0f, y + 1.5f, hp, healthColor(frac), 10.5f);

            char dist[24];
            std::snprintf(dist, sizeof(dist), "%dm", static_cast<int>(e->distance));
            float dw = rnd::textWidth(dist, 10.5f);
            rnd::text(x + w - 12.0f - dw, y + 1.5f, dist, kHudFaint, 10.5f);
            y += rowH;
        }
    }

    // ---- crosshair ----
    if (g.crosshair) {
        const float cx = std::floor(vp.x + vp.w * 0.5f);
        const float cy = std::floor(vp.y + vp.h * 0.5f);
        const float s = std::max(2.0f, std::floor(g.crosshairSize));
        const float gap = s * 0.7f;
        const uint32_t c = 0xE6FFFFFF;
        glowLine(cx - gap - s, cy, cx - gap, cy, c, 1.5f);
        glowLine(cx + gap, cy, cx + gap + s, cy, c, 1.5f);
        glowLine(cx, cy - gap - s, cx, cy - gap, c, 1.5f);
        glowLine(cx, cy + gap, cx, cy + gap + s, c, 1.5f);
        rnd::rect(cx - 1.0f, cy - 1.0f, 2.0f, 2.0f, c, 0.0f);
    }

    // The ring follows its own toggle: it must stay visible with the menu
    // closed and the aimbot off, otherwise the setting looks broken.
    if (g.aimFovCircle) {
        const float cx = vp.x + vp.w * 0.5f;
        const float cy = vp.y + vp.h * 0.5f;
        float radiusPx = (vp.h * 0.5f) *
                         std::tan(g.aimFov * 0.5f * 3.14159265358979323846f / 180.0f);
        radiusPx = std::clamp(radiusPx, 10.0f, vp.h);

        const uint32_t ring = withAlpha(argbOf(g.aimFovColor), 0.70f);
        // dashed ring: solid rings fight with the game's own HUD
        constexpr int kDashes = 64;
        for (int i = 0; i < kDashes; i += 2) {
            float a0 = (static_cast<float>(i) / kDashes) * 6.2831853f;
            float a1 = (static_cast<float>(i + 1) / kDashes) * 6.2831853f;
            rnd::line(cx + std::cos(a0) * radiusPx, cy + std::sin(a0) * radiusPx,
                      cx + std::cos(a1) * radiusPx, cy + std::sin(a1) * radiusPx,
                      0xB0000000, 2.6f);
            rnd::line(cx + std::cos(a0) * radiusPx, cy + std::sin(a0) * radiusPx,
                      cx + std::cos(a1) * radiusPx, cy + std::sin(a1) * radiusPx,
                      ring, 1.2f);
        }
    }
}

} // namespace

namespace {

std::wstring configFile() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf, n);
    const size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p.resize(slash + 1);
    return p + L"config.ini";
}

void writeBool(FILE* f, const char* k, bool v) { std::fprintf(f, "%s=%d\n", k, v ? 1 : 0); }
void writeInt(FILE* f, const char* k, int v) { std::fprintf(f, "%s=%d\n", k, v); }
void writeFloat(FILE* f, const char* k, float v) { std::fprintf(f, "%s=%.4f\n", k, v); }

bool keyIs(const char* key, const char* name) { return std::strcmp(key, name) == 0; }

} // namespace

bool saveConfig() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, configFile().c_str(), L"w") != 0 || !f) return false;

    std::fprintf(f, "[elyyykkk]\n");
    writeBool(f, "esp", g.esp);
    writeBool(f, "espBox", g.espBox);
    writeInt(f, "espStyle", g.espStyle);
    writeBool(f, "espFill", g.espFill);
    writeBool(f, "espSkeleton", g.espSkeleton);
    writeFloat(f, "espSkeletonThickness", g.espSkeletonThickness);
    writeBool(f, "espHeadDot", g.espHeadDot);
    writeBool(f, "espName", g.espName);
    writeBool(f, "espNameBg", g.espNameBg);
    writeBool(f, "espHealth", g.espHealth);
    writeBool(f, "espHealthText", g.espHealthText);
    writeBool(f, "espHealthGradient", g.espHealthGradient);
    writeBool(f, "espMaxHealth", g.espMaxHealth);
    writeInt(f, "espHealthSide", g.espHealthSide);
    writeBool(f, "espWeapon", g.espWeapon);
    writeBool(f, "espDistance", g.espDistance);
    writeBool(f, "espTracer", g.espTracer);
    writeBool(f, "espOffscreen", g.espOffscreen);
    writeBool(f, "espList", g.espList);
    writeBool(f, "espTeamCheck", g.espTeamCheck);
    writeFloat(f, "espMaxDistance", g.espMaxDistance);
    writeBool(f, "espSelf", g.espSelf);
    for (int i = 0; i < 4; ++i) {
        char k[32];
        std::snprintf(k, sizeof(k), "espBoxColor%d", i);
        writeFloat(f, k, g.espBoxColor[i]);
        std::snprintf(k, sizeof(k), "espTeamColor%d", i);
        writeFloat(f, k, g.espTeamColor[i]);
    }

    writeBool(f, "espMobs", g.espMobs);
    writeBool(f, "espMobBox", g.espMobBox);
    writeInt(f, "espMobStyle", g.espMobStyle);
    writeBool(f, "espMobSkeleton", g.espMobSkeleton);
    writeBool(f, "espMobName", g.espMobName);
    writeBool(f, "espMobHealth", g.espMobHealth);
    writeBool(f, "espMobDistance", g.espMobDistance);
    writeBool(f, "espMobLabels", g.espMobLabels);
    writeFloat(f, "espMobMaxDistance", g.espMobMaxDistance);
    for (int i = 0; i < 4; ++i) {
        char k[32];
        std::snprintf(k, sizeof(k), "espMobColor%d", i);
        writeFloat(f, k, g.espMobColor[i]);
        std::snprintf(k, sizeof(k), "espMobBossColor%d", i);
        writeFloat(f, k, g.espMobBossColor[i]);
    }

    writeBool(f, "aim", g.aim);
    writeBool(f, "aimTeamCheck", g.aimTeamCheck);
    writeBool(f, "aimVisibleOnly", g.aimVisibleOnly);
    writeBool(f, "aimMobs", g.aimMobs);
    writeFloat(f, "aimFov", g.aimFov);
    writeFloat(f, "aimSmooth", g.aimSmooth);
    writeInt(f, "aimKey", g.aimKey);
    writeInt(f, "aimMode", g.aimMode);
    writeInt(f, "aimBone", g.aimBone);
    writeInt(f, "aimPriority", g.aimPriority);
    writeFloat(f, "aimMaxDistance", g.aimMaxDistance);
    writeFloat(f, "aimPrediction", g.aimPrediction);
    writeFloat(f, "aimHumanize", g.aimHumanize);
    writeBool(f, "aimSticky", g.aimSticky);
    writeBool(f, "aimFovCircle", g.aimFovCircle);
    writeBool(f, "aimTriggerbot", g.aimTriggerbot);
    writeFloat(f, "aimTriggerRadius", g.aimTriggerRadius);
    for (int i = 0; i < 4; ++i) {
        char k[32];
        std::snprintf(k, sizeof(k), "aimFovColor%d", i);
        writeFloat(f, k, g.aimFovColor[i]);
    }

    writeBool(f, "fullbright", g.fullbright);
    writeFloat(f, "fullbrightValue", g.fullbrightValue);
    writeBool(f, "noFog", g.noFog);
    writeBool(f, "fovChanger", g.fovChanger);
    writeFloat(f, "fovValue", g.fovValue);
    writeBool(f, "crosshair", g.crosshair);
    writeFloat(f, "crosshairSize", g.crosshairSize);

    writeBool(f, "walkSpeed", g.walkSpeed);
    writeFloat(f, "walkSpeedValue", g.walkSpeedValue);
    writeBool(f, "jumpPower", g.jumpPower);
    writeFloat(f, "jumpPowerValue", g.jumpPowerValue);

    writeBool(f, "antiAfk", g.antiAfk);
    writeBool(f, "autoClick", g.autoClick);
    writeInt(f, "autoClickKey", g.autoClickKey);
    writeFloat(f, "autoClickCps", g.autoClickCps);
    for (int i = 0; i < 3; ++i) {
        char k[32];
        std::snprintf(k, sizeof(k), "waypointSet%d", i);
        writeBool(f, k, g.waypointSet[i]);
        for (int a = 0; a < 3; ++a) {
            std::snprintf(k, sizeof(k), "waypoint%d_%d", i, a);
            writeFloat(f, k, g.waypoint[i][a]);
        }
    }

    writeBool(f, "watermark", g.watermark);
    writeBool(f, "fpsCounter", g.fpsCounter);

    std::fclose(f);
    return true;
}

bool loadConfig() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, configFile().c_str(), L"r") != 0 || !f) return false;

    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        char* eq = std::strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';

        char* key = line;
        char* val = eq + 1;
        while (*val == ' ' || *val == '\t') ++val;
        char* end = val + std::strlen(val);
        while (end > val && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '))
            *--end = '\0';

        const bool b = std::atoi(val) != 0;
        const int i = std::atoi(val);
        const float v = static_cast<float>(std::atof(val));

        if (keyIs(key, "esp")) g.esp = b;
        else if (keyIs(key, "espBox")) g.espBox = b;
        else if (keyIs(key, "espStyle")) g.espStyle = std::clamp(i, 0, 2);
        else if (keyIs(key, "espFill")) g.espFill = b;
        else if (keyIs(key, "espSkeleton")) g.espSkeleton = b;
        else if (keyIs(key, "espSkeletonThickness")) g.espSkeletonThickness = std::clamp(v, 0.8f, 4.0f);
        else if (keyIs(key, "espHeadDot")) g.espHeadDot = b;
        else if (keyIs(key, "espName")) g.espName = b;
        else if (keyIs(key, "espNameBg")) g.espNameBg = b;
        else if (keyIs(key, "espHealth")) g.espHealth = b;
        else if (keyIs(key, "espHealthText")) g.espHealthText = b;
        else if (keyIs(key, "espHealthGradient")) g.espHealthGradient = b;
        else if (keyIs(key, "espMaxHealth")) g.espMaxHealth = b;
        else if (keyIs(key, "espHealthSide")) g.espHealthSide = std::clamp(i, 0, 1);
        else if (keyIs(key, "espWeapon")) g.espWeapon = b;
        else if (keyIs(key, "espDistance")) g.espDistance = b;
        else if (keyIs(key, "espTracer")) g.espTracer = b;
        else if (keyIs(key, "espOffscreen")) g.espOffscreen = b;
        else if (keyIs(key, "espList")) g.espList = b;
        else if (keyIs(key, "espTeamCheck")) g.espTeamCheck = b;
        else if (keyIs(key, "espMaxDistance")) g.espMaxDistance = std::clamp(v, 50.0f, 5000.0f);
        else if (keyIs(key, "espSelf")) g.espSelf = b;
        else if (keyIs(key, "espBoxColor0")) g.espBoxColor[0] = std::clamp(v, 0.0f, 1.0f);
        else if (keyIs(key, "espBoxColor1")) g.espBoxColor[1] = std::clamp(v, 0.0f, 1.0f);
        else if (keyIs(key, "espBoxColor2")) g.espBoxColor[2] = std::clamp(v, 0.0f, 1.0f);
        else if (keyIs(key, "espBoxColor3")) g.espBoxColor[3] = std::clamp(v, 0.0f, 1.0f);
        else if (keyIs(key, "espTeamColor0")) g.espTeamColor[0] = std::clamp(v, 0.0f, 1.0f);
        else if (keyIs(key, "espTeamColor1")) g.espTeamColor[1] = std::clamp(v, 0.0f, 1.0f);
        else if (keyIs(key, "espTeamColor2")) g.espTeamColor[2] = std::clamp(v, 0.0f, 1.0f);
        else if (keyIs(key, "espTeamColor3")) g.espTeamColor[3] = std::clamp(v, 0.0f, 1.0f);
        else if (keyIs(key, "espMobs")) g.espMobs = b;
        else if (keyIs(key, "espMobBox")) g.espMobBox = b;
        else if (keyIs(key, "espMobStyle")) g.espMobStyle = std::clamp(i, 0, 2);
        else if (keyIs(key, "espMobSkeleton")) g.espMobSkeleton = b;
        else if (keyIs(key, "espMobName")) g.espMobName = b;
        else if (keyIs(key, "espMobHealth")) g.espMobHealth = b;
        else if (keyIs(key, "espMobDistance")) g.espMobDistance = b;
        else if (keyIs(key, "espMobLabels")) g.espMobLabels = b;
        else if (keyIs(key, "espMobMaxDistance")) g.espMobMaxDistance = std::clamp(v, 50.0f, 5000.0f);
        else if (keyIs(key, "espMobColor0")) g.espMobColor[0] = std::clamp(v, 0.0f, 1.0f);
        else if (keyIs(key, "espMobColor1")) g.espMobColor[1] = std::clamp(v, 0.0f, 1.0f);
        else if (keyIs(key, "espMobColor2")) g.espMobColor[2] = std::clamp(v, 0.0f, 1.0f);
        else if (keyIs(key, "espMobColor3")) g.espMobColor[3] = std::clamp(v, 0.0f, 1.0f);
        else if (keyIs(key, "espMobBossColor0")) g.espMobBossColor[0] = std::clamp(v, 0.0f, 1.0f);
        else if (keyIs(key, "espMobBossColor1")) g.espMobBossColor[1] = std::clamp(v, 0.0f, 1.0f);
        else if (keyIs(key, "espMobBossColor2")) g.espMobBossColor[2] = std::clamp(v, 0.0f, 1.0f);
        else if (keyIs(key, "espMobBossColor3")) g.espMobBossColor[3] = std::clamp(v, 0.0f, 1.0f);
        else if (keyIs(key, "aim")) g.aim = b;
        else if (keyIs(key, "aimTeamCheck")) g.aimTeamCheck = b;
        else if (keyIs(key, "aimVisibleOnly")) g.aimVisibleOnly = b;
        else if (keyIs(key, "aimMobs")) g.aimMobs = b;
        else if (keyIs(key, "aimFov")) g.aimFov = std::clamp(v, 5.0f, 360.0f);
        else if (keyIs(key, "aimSmooth")) g.aimSmooth = std::clamp(v, 1.0f, 40.0f);
        else if (keyIs(key, "aimKey")) g.aimKey = i;
        else if (keyIs(key, "aimMode")) g.aimMode = std::clamp(i, 0, 1);
        else if (keyIs(key, "aimBone")) g.aimBone = std::clamp(i, 0, 2);
        else if (keyIs(key, "aimPriority")) g.aimPriority = std::clamp(i, 0, 2);
        else if (keyIs(key, "aimMaxDistance")) g.aimMaxDistance = std::clamp(v, 50.0f, 3000.0f);
        else if (keyIs(key, "aimPrediction")) g.aimPrediction = std::clamp(v, 0.0f, 4.0f);
        else if (keyIs(key, "aimHumanize")) g.aimHumanize = std::clamp(v, 0.0f, 6.0f);
        else if (keyIs(key, "aimSticky")) g.aimSticky = b;
        else if (keyIs(key, "aimFovCircle")) g.aimFovCircle = b;
        else if (keyIs(key, "aimTriggerbot")) g.aimTriggerbot = b;
        else if (keyIs(key, "aimTriggerRadius")) g.aimTriggerRadius = std::clamp(v, 2.0f, 60.0f);
        else if (keyIs(key, "aimFovColor0")) g.aimFovColor[0] = std::clamp(v, 0.0f, 1.0f);
        else if (keyIs(key, "aimFovColor1")) g.aimFovColor[1] = std::clamp(v, 0.0f, 1.0f);
        else if (keyIs(key, "aimFovColor2")) g.aimFovColor[2] = std::clamp(v, 0.0f, 1.0f);
        else if (keyIs(key, "aimFovColor3")) g.aimFovColor[3] = std::clamp(v, 0.0f, 1.0f);
        else if (keyIs(key, "fullbright")) g.fullbright = b;
        else if (keyIs(key, "fullbrightValue")) g.fullbrightValue = std::clamp(v, 0.0f, 10.0f);
        else if (keyIs(key, "noFog")) g.noFog = b;
        else if (keyIs(key, "fovChanger")) g.fovChanger = b;
        else if (keyIs(key, "fovValue")) g.fovValue = std::clamp(v, 30.0f, 120.0f);
        else if (keyIs(key, "crosshair")) g.crosshair = b;
        else if (keyIs(key, "crosshairSize")) g.crosshairSize = std::clamp(v, 3.0f, 24.0f);
        else if (keyIs(key, "walkSpeed")) g.walkSpeed = b;
        else if (keyIs(key, "walkSpeedValue")) g.walkSpeedValue = std::clamp(v, 16.0f, 500.0f);
        else if (keyIs(key, "jumpPower")) g.jumpPower = b;
        else if (keyIs(key, "jumpPowerValue")) g.jumpPowerValue = std::clamp(v, 50.0f, 500.0f);
        else if (keyIs(key, "antiAfk")) g.antiAfk = b;
        else if (keyIs(key, "autoClick")) g.autoClick = b;
        else if (keyIs(key, "autoClickKey")) g.autoClickKey = i;
        else if (keyIs(key, "autoClickCps")) g.autoClickCps = std::clamp(v, 1.0f, 25.0f);
        else if (keyIs(key, "waypointSet0")) g.waypointSet[0] = b;
        else if (keyIs(key, "waypointSet1")) g.waypointSet[1] = b;
        else if (keyIs(key, "waypointSet2")) g.waypointSet[2] = b;
        else if (keyIs(key, "waypoint0_0")) g.waypoint[0][0] = v;
        else if (keyIs(key, "waypoint0_1")) g.waypoint[0][1] = v;
        else if (keyIs(key, "waypoint0_2")) g.waypoint[0][2] = v;
        else if (keyIs(key, "waypoint1_0")) g.waypoint[1][0] = v;
        else if (keyIs(key, "waypoint1_1")) g.waypoint[1][1] = v;
        else if (keyIs(key, "waypoint1_2")) g.waypoint[1][2] = v;
        else if (keyIs(key, "waypoint2_0")) g.waypoint[2][0] = v;
        else if (keyIs(key, "waypoint2_1")) g.waypoint[2][1] = v;
        else if (keyIs(key, "waypoint2_2")) g.waypoint[2][2] = v;
        else if (keyIs(key, "watermark")) g.watermark = b;
        else if (keyIs(key, "fpsCounter")) g.fpsCounter = b;
    }
    std::fclose(f);
    return true;
}

void resetDefaults() { g = Settings{}; }

void toast(const char* text) {
    std::snprintf(g_toast, sizeof(g_toast), "%s", text);
    g_toastUntil = GetTickCount64() + 2200;
}

float fps() { return g_fps; }

void shutdown() {
    saveConfig();
    restoreVisuals();
}

void setDiag(bool on) {
    g_diag = on;
    if (on) {
        FILE* f = nullptr;
        fopen_s(&f, "diag.txt", "w");
        if (f) { std::fprintf(f, "ELYYYKKK diag\n"); std::fclose(f); }
    }
}

void update(int surfaceW, int surfaceH) {
    (void)surfaceW;
    (void)surfaceH;
    ULONGLONG now = GetTickCount64();
    if (!g_fpsStart) g_fpsStart = now;
    g_fpsFrames++;
    ULONGLONG elapsed = now - g_fpsStart;
    if (elapsed >= 500) {
        g_fps = g_fpsFrames * 1000.0f / static_cast<float>(elapsed);
        g_fpsFrames = 0;
        g_fpsStart = now;
    }

    // Keep the last entity snapshot and team across frames: the refresh below
    // is rate-limited, and clearing here made ESP blink at the refresh rate.
    g_camValid = false;

    if (!mem::g.valid()) {
        // keep the menu alive while Roblox boots; retry the attach
        static ULONGLONG lastAttempt = 0;
        if (now - lastAttempt > 2000) {
            lastAttempt = now;
            mem::g.attach(off::ProcessName);
        }
        return;
    }

    // A single failed camera read must not blank a frame (that shows up as a
    // one-frame ESP blink), but a persistent failure has to hide the ESP.
    static int camFailRun = 0;
    if (game::readCamera(g_cam)) {
        g_camValid = true;
        camFailRun = 0;
    } else if (g_camValid && ++camFailRun < 30) {
        // keep last frame's camera and keep drawing
    } else {
        g_camValid = false;
    }
    if (!g_camValid) return;

    // Refresh cadence is decided from a smoothed walk cost, not from the last
    // walk: a per-frame decision flip-flops between "every frame" and "every
    // other frame" as the player count changes, and that alternation reads as
    // judder/flicker even though nothing is actually blanked.
    static std::chrono::steady_clock::time_point lastRefresh{};
    static std::chrono::steady_clock::time_point nextAllowed{};
    static float avgCostMs = 0.0f;

    auto t0 = std::chrono::steady_clock::now();
    if (lastRefresh.time_since_epoch().count() == 0 || t0 >= nextAllowed) {
        g_localTeam = game::localTeamAddress();
        {
            rb::Instance lp = rb::localPlayer();
            g_localPlayer = lp ? lp.address() : 0;
            // Kit signature of our own side, for games with no Roblox Team. It
            // is kept across death so team colours still work while spectating;
            // it is only replaced when the local player is actually alive, and
            // cleared when the game no longer exists (left the place).
            if (lp) {
                const uintptr_t m =
                    mem::g.read<uintptr_t>(lp.address() + off::PlayerModel);
                if (m) {
                    const std::string sig = game::teamSignature(m);
                    if (!sig.empty()) g_localTeamSig = sig;
                }
            }
        }
        size_t seen = 0;
        auto snapshot = game::collectEntities(g_cam, g.espSelf, &seen);
        if (seen == 0) g_localTeamSig.clear();

        // Never let a transient bad walk (a vector being reallocated mid-read, a
        // respawn) blank a good snapshot: that is exactly "ESP blinks while it
        // still tracks the player". Only clear when the game really has no
        // players (we left the place), or after ~a third of a second, so ghosts
        // cannot stick around either.
        bool accept = !snapshot.empty() || seen == 0 || g_entities.empty() || g_keepStale >= 20;
        if (accept) {
            if (g_lastEntCount >= 2 && static_cast<int>(snapshot.size()) < g_lastEntCount - 1)
                ++g_diagEntDrop;
            g_lastEntCount = static_cast<int>(snapshot.size());
            g_keepStale = 0;
            g_entities.swap(snapshot);

            // Only trust the head-gear split when both sides are really present;
            // in a game where every player shares one head piece it would label
            // the whole server as friendly.
            bool has1 = false, has2 = false;
            for (const auto& e : g_entities) {
                const int s = headSide(e.teamSig);
                if (s == 1) has1 = true;
                else if (s == 2) has2 = true;
            }
            g_headSplit = has1 && has2;
        } else {
            ++g_keepStale;
            ++g_diagEntDrop;
        }

        // Feed the mob walker the exact player models so it never classifies a
        // player character as an NPC. The local model goes in even when espSelf
        // is off.
        {
            std::vector<uintptr_t> chars;
            chars.reserve(g_entities.size() + 1);
            const uintptr_t localModel =
                g_localPlayer ? mem::g.read<uintptr_t>(g_localPlayer + off::PlayerModel) : 0;
            if (localModel) chars.push_back(localModel);
            for (const auto& e : g_entities)
                if (e.characterAddr) chars.push_back(e.characterAddr);
            game::setPlayerCharacters(chars);
        }
        game::setReadTools(g.espWeapon);

        if (g.espMobs || g.aimMobs || (g.aim && g.aimVisibleOnly)) {
            size_t mobsSeen = 0;
            g_mobs = game::collectMobs(g_cam, &mobsSeen);
        } else {
            g_mobs.clear();
        }

        // One merged view for the ESP, aimbot and entity list. The strings are
        // already in the snapshots; the copy is cheap next to the reads.
        g_all.clear();
        g_all.reserve(g_entities.size() + g_mobs.size());
        for (const auto& e : g_entities) g_all.push_back(e);
        for (const auto& e : g_mobs) g_all.push_back(e);

        auto costUs = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - t0).count();
        avgCostMs = avgCostMs * 0.85f + static_cast<float>(costUs) * 0.001f * 0.15f;
        g_diagCostMs = avgCostMs;
        // Stay on "every frame" up to an 8 ms walk; go to every other frame only
        // if the walk is genuinely expensive.
        auto interval = (avgCostMs > 8.0f) ? std::chrono::milliseconds(16)
                                          : std::chrono::milliseconds(0);
        nextAllowed = std::chrono::steady_clock::now() + interval;
        lastRefresh = std::chrono::steady_clock::now();
    }

    // Read the aim key once per frame so hold and toggle both see the same
    // state, and the visible-only cache warms exactly when aim is actually live.
    const bool aiming = aimActive();

    // Keep the occluder cache warm while "visible only" is armed, so the first
    // lock after the key goes down does not have to wait for a walk.
    if (g.aim && g.aimVisibleOnly) {
        // Hand the cache the exact character models. Several FPS games use a
        // custom rig with an AnimationController instead of a Humanoid and
        // parent characters under a folder, so class-based detection misses them
        // and their own hitboxes would block every ray.
        std::vector<uintptr_t> chars;
        chars.reserve(g_all.size() + 1);
        const uintptr_t localModel =
            g_localPlayer ? mem::g.read<uintptr_t>(g_localPlayer + off::PlayerModel) : 0;
        if (localModel) chars.push_back(localModel);
        for (const auto& e : g_all)
            if (e.characterAddr) chars.push_back(e.characterAddr);
        world::setCharacters(chars);

        world::update(aiming);
    }

    if (g.aim) {
        const auto aimStart = std::chrono::steady_clock::now();
        runAimbot(g_cam, g_all, game::robloxViewport(), g_localTeam, aiming);
        const float aimMs = std::chrono::duration<float, std::milli>(
                                std::chrono::steady_clock::now() - aimStart).count();
        g_diagAimMs = g_diagAimMs * 0.9f + aimMs * 0.1f;
    }

    applyVisuals();
    applyMovement();
    applyUtility();
}

void render(int surfaceW, int surfaceH) {
    (void)surfaceH;
    float sw = static_cast<float>(surfaceW);
    game::Viewport vp = game::robloxViewport();

    int boxes = 0;
    if (mem::g.valid() && g_camValid)
        boxes = drawESP(g_cam, g_all, vp, g_localTeam);

    diagTick(g_camValid, boxes, static_cast<int>(g_all.size()));

    drawOverlays(vp, sw);

    // The panel is the *menu*; F1 must actually show/hide it. Widgets are only
    // fed input while it is open, so a hidden menu cannot swallow clicks.
    if (overlay::menuOpen()) drawMenu();
}

} // namespace feat
