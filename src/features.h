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
    bool espHeadDot = true;
    bool espName = true;
    bool espNameBg = false;        // plate behind the name (off = outlined text)
    bool espHealth = true;
    bool espHealthText = true;
    bool espDistance = true;
    bool espTracer = false;
    bool espList = false;          // player list panel
    bool espTeamCheck = true;
    float espMaxDistance = 1000.0f;
    bool espSelf = false;          // draw the local player too (alignment test)
    float espBoxColor[4] = {0.20f, 0.85f, 1.00f, 1.00f};
    float espTeamColor[4] = {1.00f, 0.45f, 0.15f, 1.00f};

    // Aimbot
    bool aim = false;
    bool aimTeamCheck = true;
    bool aimVisibleOnly = false;
    float aimFov = 120.0f;         // degrees
    float aimSmooth = 3.0f;        // >1 = slower, 1 = snap
    int aimKey = VK_XBUTTON2;      // mouse5
    int aimMode = 0;               // 0 = hold, 1 = toggle
    int aimBone = 0;               // 0 head, 1 torso, 2 root
    float aimMaxDistance = 900.0f;
    float aimPrediction = 0.0f;    // studs of lead, 0 = off
    bool aimSticky = true;         // keep the current target while it stays valid
    bool aimFovCircle = true;      // draw the aim FOV circle on screen

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
