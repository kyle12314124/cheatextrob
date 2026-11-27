#pragma once
#include <windows.h>

namespace feat {

struct Settings {
    // ESP
    bool esp = true;
    bool espBox = true;
    int espStyle = 0;              // 0 box, 1 corners, 2 3D box
    bool espFill = false;          // translucent fill inside the box
    bool espSkeleton = false;
    float espSkeletonThickness = 1.4f;
    bool espHeadDot = true;
    bool espName = true;
    bool espNameBg = false;        // plate behind the name (off = outlined text)
    bool espHealth = true;
    bool espHealthText = true;
    bool espHealthGradient = false;
    bool espMaxHealth = false;     // "hp / max" instead of just hp
    int espHealthSide = 0;         // 0 left, 1 right
    bool espWeapon = false;        // equipped Tool name under the name
    bool espDistance = true;
    bool espTracer = false;
    bool espOffscreen = false;     // arrows at the viewport edge for off-screen targets
    bool espList = false;          // player list panel
    bool espTeamCheck = true;
    float espMaxDistance = 1000.0f;
    bool espSelf = false;          // draw the local player too (alignment test)
    float espBoxColor[4] = {0.20f, 0.85f, 1.00f, 1.00f};
    float espTeamColor[4] = {1.00f, 0.45f, 0.15f, 1.00f};

    // Mobs / NPCs
    bool espMobs = true;
    bool espMobBox = true;
    int espMobStyle = 0;           // 0 box, 1 corners, 2 3D box
    bool espMobSkeleton = false;
    bool espMobName = true;
    bool espMobHealth = true;
    bool espMobDistance = false;
    bool espMobLabels = true;      // classification tag: zombie, boss, npc...
    float espMobMaxDistance = 1000.0f;
    float espMobColor[4] = {1.00f, 0.28f, 0.36f, 1.00f};
    float espMobBossColor[4] = {1.00f, 0.78f, 0.15f, 1.00f};

    // Aimbot
    bool aim = false;
    bool aimTeamCheck = true;
    bool aimVisibleOnly = false;
    bool aimMobs = false;
    float aimFov = 120.0f;         // degrees
    float aimSmooth = 3.0f;        // >1 = slower, 1 = snap
    int aimKey = VK_XBUTTON2;      // mouse5
    int aimMode = 0;               // 0 = hold, 1 = toggle
    int aimBone = 0;               // 0 head, 1 torso, 2 root
    int aimPriority = 0;           // 0 crosshair, 1 distance, 2 lowest health
    float aimMaxDistance = 900.0f;
    float aimPrediction = 0.0f;    // studs of lead, 0 = off
    float aimHumanize = 0.0f;      // px of random jitter on the final step
    bool aimSticky = true;         // keep the current target while it stays valid
    bool aimFovCircle = true;      // draw the aim FOV circle on screen
    float aimFovColor[4] = {0.37f, 0.62f, 1.00f, 1.00f};
    bool aimTriggerbot = false;    // auto-fire when the crosshair is on the target
    float aimTriggerRadius = 10.0f;

    // Visuals
    bool fullbright = false;
    float fullbrightValue = 3.0f;
    bool noFog = false;
    bool fovChanger = false;
    float fovValue = 70.0f;
    bool crosshair = false;        // custom crosshair over the viewport centre
    float crosshairSize = 8.0f;

    // Movement
    bool walkSpeed = false;
    float walkSpeedValue = 32.0f;
    bool jumpPower = false;
    float jumpPowerValue = 100.0f;

    // Utility
    bool antiAfk = false;
    bool autoClick = false;
    int autoClickKey = 'C';
    float autoClickCps = 10.0f;
    bool waypointSet[3] = {false, false, false};
    float waypoint[3][3] = {};

    bool watermark = true;
    bool fpsCounter = true;
};

extern Settings g;

void update(int surfaceW, int surfaceH);   // aimbot + memory writes
void render(int surfaceW, int surfaceH);   // ESP + menu
void setDiag(bool on);                     // --diag: per-second frame counters
void shutdown();                           // restore FOV / lighting on exit

bool saveConfig();                         // config.ini beside the exe
bool loadConfig();
void resetDefaults();
void toast(const char* text);              // short message in the sidebar footer

float fps();

} // namespace feat
