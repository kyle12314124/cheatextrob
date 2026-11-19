#pragma once
#include <cstdint>

// Source: theo's external offsets, RbxDumperV2 2.2.5
// Roblox version-2c6fd5e09aaf4cfa, dumped 17/11/2026.
// Everything version-dependent lives here and nowhere else.
// Refresh with: RobloxExt.exe --scan

namespace off {

// ---- version this block was dumped for ----
inline constexpr const char* ClientVersion = "version-4ea8e85481e34f46";
inline constexpr wchar_t ProcessName[] = L"RobloxPlayerBeta.exe";
inline constexpr wchar_t WindowClass[] = L"WINDOWSCLIENT";

// ---- DataModel chain ----
// One slot per known client build; resolution tries each until the pointer
// validates. Add a row when theo publishes a new version.
struct DataModelSlot {
    const char* version;
    uintptr_t pointer;
    uintptr_t realOffset;
};

inline constexpr DataModelSlot KnownSlots[] = {
    {"version-4ea8e85481e34f46", 0x8f8e968, 0x1f8},
    {"version-2c6fd5e09aaf4cfa", 0x8e16368, 0x1f8},
};

inline constexpr uintptr_t RealDataModel = 0x1f8;

// ---- Instance layout ----
// Children is a pointer at +0x70 to a small container object:
//   container+0x00 = first child slot, container+0x08 = one past the end
// Each slot is an 8-byte Instance* whose +0x60 parent points back at us.
// (the dump lists ChildrenEnd = 0x8, i.e. relative to that container)
inline constexpr uintptr_t InstanceChildren       = 0x70;
inline constexpr uintptr_t InstanceChildrenBegin  = 0x00;
inline constexpr uintptr_t InstanceChildrenEnd    = 0x08;
inline constexpr uintptr_t InstanceName     = 0x68;  // std::string*
inline constexpr uintptr_t InstanceDescriptor = 0x18; // ClassDescriptor*
inline constexpr uintptr_t DescriptorClassName = 0x08; // std::string*
inline constexpr uintptr_t InstanceParent   = 0x60;

// ---- BasePart / Primitive ----
inline constexpr uintptr_t PartPrimitive    = 0x178; // Primitive*
inline constexpr uintptr_t PrimitivePosition = 0xd4; // Vector3
inline constexpr uintptr_t PrimitiveRotation = 0xb0; // 3x3
inline constexpr uintptr_t PrimitiveSize     = 0x1c4; // Vector3

// ---- DataModel / Workspace ----
inline constexpr uintptr_t DataModelWorkspace = 0x168;
inline constexpr uintptr_t WorkspaceCamera    = 0x4a0;

// ---- Player ----
inline constexpr uintptr_t PlayerModel      = 0x2a0; // Model*
inline constexpr uintptr_t PlayerTeam       = 0x2e0; // Team*
inline constexpr uintptr_t PlayerUserId     = 0xb8;
inline constexpr uintptr_t PlayersLocal     = 0x118;

// ---- Humanoid ----
inline constexpr uintptr_t HumanoidHealth    = 0x184;
inline constexpr uintptr_t HumanoidMaxHealth = 0x1a0;
inline constexpr uintptr_t HumanoidWalkSpeed = 0x1c8;
inline constexpr uintptr_t HumanoidJumpPower = 0x19c;
inline constexpr uintptr_t HumanoidHipHeight = 0x188;

// ---- Camera ----
inline constexpr uintptr_t CameraRotation     = 0xc0; // 3x3 floats
inline constexpr uintptr_t CameraPosition     = 0xe4; // Vector3
inline constexpr uintptr_t CameraFOV          = 0x128;
inline constexpr uintptr_t CameraViewportSize = 0x2c4;

// ---- Lighting ----
inline constexpr uintptr_t LightingBrightness = 0x100;
inline constexpr uintptr_t LightingFogEnd     = 0x114;

} // namespace off
