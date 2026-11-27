#include "memory.h"
#include "instance.h"
#include "offsets.h"
#include "overlay.h"
#include "features.h"
#include "game.h"
#include "world.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <windows.h>
#include <shellapi.h>

namespace {

FILE* g_report = nullptr;
bool  g_console = false;

void out(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_console) std::fputs(buf, stdout);
    if (g_report) std::fputs(buf, g_report);
}

void waitForProcess(int timeoutMs) {
    using namespace std::chrono;
    auto start = steady_clock::now();
    while (!mem::g.attach(off::ProcessName)) {
        if (duration_cast<milliseconds>(steady_clock::now() - start).count() > timeoutMs)
            return;
        std::this_thread::sleep_for(milliseconds(500));
    }
}

// --scan: validates the offset block and prints the live chain.
// Run after every Roblox update, then patch src/offsets.h.
int scan() {
    g_report = nullptr;
    fopen_s(&g_report, "offsets_report.txt", "w");

    out("ELYYYKKK // offset report\n");
    out("---------------------------\n");

    if (!mem::g.attach(off::ProcessName)) {
        out("process: RobloxPlayerBeta.exe not found\n");
        if (g_report) fclose(g_report);
        return 1;
    }

    out("pid:          %u\n", mem::g.pid());

    std::wstring modPath = mem::g.modulePath(off::ProcessName);
    size_t vpos = modPath.find(L"version-");
    std::wstring running = (vpos != std::wstring::npos) ? modPath.substr(vpos, 24) : L"unknown";
    std::string runningUtf8;
    for (wchar_t c : running)
        runningUtf8.push_back(static_cast<char>(c & 0x7F));
    out("running:      %s\n", runningUtf8.c_str());
    out("offsets for:  %s\n", off::ClientVersion);
    out("module base:  0x%016llX\n", static_cast<unsigned long long>(mem::g.base()));
    out("module size:  0x%llX\n",
        static_cast<unsigned long long>(mem::g.moduleSize(off::ProcessName)));

    rb::Instance dm = rb::dataModel();
    out("datamodel:    0x%016llX  valid=%d\n",
        static_cast<unsigned long long>(dm.address()), dm ? 1 : 0);
    out("slot matched: %s\n", rb::matchedVersion() ? rb::matchedVersion() : "none");
    out("fake offset:  0x%llX  (0 = auto-discovered)\n",
        static_cast<unsigned long long>(rb::discoveredFakeOffset()));

    int rc = 0;
    if (dm) {
        auto kids = dm.children();
        out("services (children=%zu):\n", kids.size());
        for (const auto& c : kids) {
            std::string cn = c.className();
            if (cn.empty()) continue;
            out("  %-28s 0x%016llX\n", cn.c_str(),
                static_cast<unsigned long long>(c.address()));
        }

        rb::Instance players = rb::players();
        if (players) {
            out("local player: 0x%016llX\n",
                static_cast<unsigned long long>(rb::localPlayer().address()));
            auto plist = players.children();
            out("player count: %zu\n", plist.size());

            // Feed the mob walker the player models so no Player character is
            // ever listed as a mob in the dump below.
            {
                std::vector<uintptr_t> pchars;
                pchars.reserve(plist.size());
                for (const auto& p : plist) {
                    uintptr_t ch = mem::g.read<uintptr_t>(p.address() + off::PlayerModel);
                    if (ch) pchars.push_back(ch);
                }
                game::setPlayerCharacters(pchars);
            }
            for (const auto& p : plist) {
                if (p.className() != "Player") continue;
                uintptr_t character = mem::g.read<uintptr_t>(p.address() + off::PlayerModel);
                out("  %-20s 0x%016llX char=0x%016llX\n",
                    p.name().c_str(),
                    static_cast<unsigned long long>(p.address()),
                    static_cast<unsigned long long>(character));
            }

            // Character/part diagnostics: in game every Player should show a
            // live Humanoid and three readable part positions.
            game::Camera scanCam{};
            bool haveCam = game::readCamera(scanCam);
            out("\ncharacters:\n");
            for (const auto& p : plist) {
                if (p.className() != "Player") continue;
                game::Entity e = game::buildEntity(p);
                if (!e.characterAddr) {
                    out("  %-20s no character (menu or still loading)\n",
                        p.name().c_str());
                    continue;
                }
                Vec3 anchor = length(e.rootPos) > 1e-3f ? e.rootPos : e.headPos;
                float dist = haveCam ? length(anchor - scanCam.pos) : 0.0f;
                out("  %-20s model=0x%016llX humanoid=0x%016llX hp=%.0f/%.0f\n",
                    e.name.c_str(), static_cast<unsigned long long>(e.characterAddr),
                    static_cast<unsigned long long>(e.humanoidAddr),
                    e.health, e.maxHealth);
                out("    head 0x%016llX pos %.1f %.1f %.1f\n",
                    static_cast<unsigned long long>(e.headAddr),
                    e.headPos.x, e.headPos.y, e.headPos.z);
                out("    root 0x%016llX pos %.1f %.1f %.1f\n",
                    static_cast<unsigned long long>(e.rootAddr),
                    e.rootPos.x, e.rootPos.y, e.rootPos.z);
                out("    torso 0x%016llX pos %.1f %.1f %.1f  dist=%.0f\n",
                    static_cast<unsigned long long>(e.torsoAddr),
                    e.torsoPos.x, e.torsoPos.y, e.torsoPos.z, dist);
                int joints = 0;
                for (int b = 0; b < game::BoneCount; ++b)
                    if (e.boneMask & (1u << b)) ++joints;
                out("    rig %s joints=%d/%d\n", e.r15 ? "R15" : "R6", joints,
                    static_cast<int>(game::BoneCount));
            }
        }

        rb::Instance cam = rb::camera();
        out("camera:       0x%016llX\n", static_cast<unsigned long long>(cam.address()));
        if (cam) {
            game::Camera gc;
            bool ok = game::readCamera(gc);
            float pos[3]{};
            mem::g.readRaw(cam.address() + off::CameraPosition, pos, sizeof(pos));
            out("  right:  %.2f %.2f %.2f\n", gc.right.x, gc.right.y, gc.right.z);
            out("  pos:    %.2f %.2f %.2f\n", pos[0], pos[1], pos[2]);
            out("  fov:    %.1f deg%s\n", ok ? gc.fov : 0.0f,
                ok ? "" : " (rotation invalid)");
        }

        // Occluder cache used by the aimbot's "visible only" test.
        auto occStart = std::chrono::steady_clock::now();
        world::buildNow();
        auto occMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - occStart).count();
        out("\noccluders: %zu cached, %zu parts walked, %lldms\n",
            world::occluders(), world::partsSeen(),
            static_cast<long long>(occMs));

        // Mob / NPC detection. collectMobsNow drives the same Workspace walk to
        // completion, so the dump lists everything the live overlay would draw.
        game::Camera mobCam{};
        game::readCamera(mobCam);
        auto mobStart = std::chrono::steady_clock::now();
        size_t mobCount = 0;
        auto mobs = game::collectMobsNow(mobCam, &mobCount);
        auto mobMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - mobStart).count();
        out("\nmobs: %zu detected (%lldms)\n", mobs.size(),
            static_cast<long long>(mobMs));
        for (const auto& e : mobs) {
            out("  %-20s [%s] model=0x%016llX hp=%.0f/%.0f dist=%.0f\n",
                e.name.empty() ? "(unnamed)" : e.name.c_str(), e.label.c_str(),
                static_cast<unsigned long long>(e.characterAddr),
                e.health, e.maxHealth, e.distance);
        }
    } else {
        rc = 2;
        out("\noffsets stale: FakeDataModel / RealDataModel / instance layout\n");
        out("refresh with ReClass or a pointer scan, see README.md\n");
    }

    if (g_report) fclose(g_report);
    return rc;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool scanMode = false;
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], L"--scan") == 0) scanMode = true;

    // Saved settings first, so launch flags can still override them.
    if (!scanMode) feat::loadConfig();

    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--esp-self") == 0) feat::g.espSelf = true;
        if (_wcsicmp(argv[i], L"--skeleton") == 0) feat::g.espSkeleton = true;
        if (_wcsicmp(argv[i], L"--crosshair") == 0) feat::g.crosshair = true;
        if (_wcsicmp(argv[i], L"--diag") == 0) feat::setDiag(true);
        if (_wcsicmp(argv[i], L"--fov") == 0 && i + 1 < argc) {
            feat::g.fovChanger = true;
            feat::g.fovValue = static_cast<float>(_wtof(argv[++i]));
        }
    }
    if (argv) LocalFree(argv);

    if (scanMode) {
        // rebind stdout only when a real console is present
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* con = nullptr;
            if (freopen_s(&con, "CONOUT$", "w", stdout) == 0 && con)
                g_console = true;
        }
        int rc = scan();
        return rc;
    }

    waitForProcess(0); // no wait: the overlay opens and attaches in the background
    int rc = overlay::run();
    mem::g.detach();
    return rc;
}
