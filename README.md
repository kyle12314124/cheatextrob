# ELYYYKKK

External ESP + aimbot for Roblox. Separate process, `ReadProcessMemory` only,
Direct2D overlay. No injection, so Hyperion's internal scanner never touches it.

## build

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output: `build/Release/RobloxExt.exe`. No third-party dependencies; Direct2D and
DirectWrite ship with Windows.

## run

1. Roblox in **borderless fullscreen on the primary monitor** (the overlay
   covers the whole screen and uses screen coordinates).
2. Start `RobloxExt.exe`. The menu opens immediately; it attaches to
   `RobloxPlayerBeta.exe` on its own and retries every 2 s.
3. `F1` opens/closes the menu. Open = panel visible, mouse usable, keyboard
   focus on the overlay. Closed = panel hidden, fully click-through, focus
   handed back to Roblox so the game keeps input.

Hotkeys:

| key | action |
|---|---|
| F1 | show/hide menu (global hotkey) |
| aim key (default mouse5) | aimbot trigger - hold or toggle, set in the Aimbot tab |

Launch flags:

| flag | action |
|---|---|
| `--scan` | write `offsets_report.txt` and exit |
| `--esp-self` | draw ESP on your own avatar (alignment self-test) |
| `--skeleton` | start with skeleton ESP on |
| `--crosshair` | start with the custom crosshair on |
| `--fov <deg>` | start with the FOV changer on at that value |
| `--diag` | append per-second frame counters to `diag.txt` |

`--diag` is how a flicker gets diagnosed without guessing. Each line reports
frames presented, how many of them drew boxes, camera read failures, the
entity count range, and the smoothed cost of the instance walk:

```
t=  12s frames= 75 esp= 75 camfail= 0 boxes=  525 ents=27..27 drop=0 fps=74 walk=2.1ms
```

`esp == frames` and `camfail=0` means the ESP was drawn every frame and any
blink you see is compositing, not the entity data.

## menu

A landscape card: tabs down the left rail, two columns of settings on the
right. The panel sizes itself to the current tab, drag it by its top edge, and
hit the switches / chips / sliders directly. `F1` is the only global key.

## settings

`config.ini` sits next to the exe. It is read at startup and written on exit,
and the **Config** tab has explicit *save / load / reset* buttons. Launch flags
are applied after the file, so a flag still wins for that run.

## features

- ESP visuals: three box styles (box / corners / 3D box), optional translucent
  fill, head ring, health bar with numeric HP, name plate, distance, tracer,
  distance fade, dark halo under every stroke so lines read on bright and dark
  backgrounds, per-side colours, team check, distance limit, optional player
  list panel.
- ESP geometry: the box is built from the **projected joints** (width, so arms
  fit) and from the **head top to the soles of the feet** (height), with the feet
  taken from the leg/foot parts - `Humanoid.HipHeight` is 0 on R6, which is why a
  root-based box used to stop at the thighs.
- Skeleton: real R6/R15 rigs. The character's children are walked once and every
  joint (head, upper/lower torso, upper/lower arm, hand, upper/lower leg, foot,
  or R6 torso/arms/legs) is resolved from the part positions, then drawn with the
  rig's own links. R6 draws shoulder and hip bars with legs extended to the feet
  instead of a head-to-root line. Custom rigs are supported too: parts named
  `LeftArm`/`LeftLeg` without a space resolve the same as `Left Arm`/`Left Leg`,
  and a character with no `Humanoid` (an `AnimationController` rig, as several
  FPS games use) still gets a full skeleton and box.
- Dead players are not drawn or locked: `Player.Character` being cleared (the
  usual sign of death in round-based games), `Humanoid.Health` at zero, or a
  custom rig lying flat all remove the body. A live crouch or swim keeps its
  `Humanoid`, so it is never mistaken for a corpse.
- Team check uses the real Roblox `Team` when the game creates one. Games that
  skip teams entirely (everything under `Workspace.Characters`, no `Team` object)
  fall back to the uniform. When the two sides wear different headgear - a
  balaclava on one side, a covered face on the other, as in Bloxstrike - that
  head piece decides the side, so armour bought on top cannot flip an enemy into
  a teammate. If no such head piece exists, the armour pieces both sides wear are
  compared by overlap instead. The local side is remembered while you are dead or
  spectating, so team colours keep working on the death cam.
- Streamed-out players stay on the ESP at their last seen position. Some games
  stop replicating a character and park the leftover copy outside Workspace
  (Bloxstrike uses `ReplicatedStorage._PVS_CulledCharacters`), so its transform
  freezes. The copy still holds the last place the client saw that player, so it
  is drawn like any other body - keeping far enemies visible instead of dropping
  them the moment they leave the PVS - while the aimbot ignores it because the
  position is no longer live. Games that never stream players (Rivals, typical
  Roblox places) are unaffected.
- Aimbot: FOV limit, smoothing, bone (head/torso/root), trigger key, team check,
  per-frame lead prediction, sticky target, and a FOV circle that is drawn from
  its own toggle - it stays on screen with the menu closed and the aimbot off.
  Smoothing is time-based, so the lock takes the same time at 60 and 144 fps,
  the first frame on a new target takes a bigger step, and a target that is not
  visible never blocks the search: the moment someone steps into view they are
  locked, with no need to re-press the key. The trigger is either held or
  toggled (Aimbot -> trigger), and the key itself is rebindable: click the
  `aim key` field, then press any key or mouse button. F1 stays reserved for the
  menu and Escape cancels the bind.
- Visible only: a target is skipped when another player's body or a piece of map
  geometry sits between the camera and the aim point. The map is cached by
  `src/world.*` - a background Workspace walk that costs a fraction of a
  millisecond per frame and re-samples parts a few at a time afterwards, so the
  check works on walls instead of only on players. The walk skips every
  character, wherever the game parents it, including rigs with no `Humanoid`
  (the player list provides the exact model addresses, and any model owning a
  `HumanoidRootPart` is treated as a rig), and geometry sitting on the player
  (the first-person weapon viewmodel, own accessories, spawned effects) is never
  treated as cover, so the lock cannot fail just because the gun model is on the
  line of fire. The menu shows the cached count under the toggle while it is on.
- Visuals: fullbright, no fog, FOV changer (writes `Camera.FieldOfView` in
  radians and restores it on exit), custom crosshair, watermark with player
  count, fps.
- Player: walkspeed, jumppower.
- `--scan` writes `offsets_report.txt` with the live chain, plus a `rig R6/R15
  joints=n` line per player so a broken skeleton is obvious without guessing, and
  an `occluders: n cached` line once the geometry cache is built.

## offsets

Everything version-dependent is in `src/offsets.h`. Source: theo's external
offsets, RbxDumperV2 2.2.5, `version-4ea8e85481e34f46` (10/11/2026), with the
instance layout re-verified live. Nothing else in the build changes when a patch
lands.

```
FakeDataModel            0x8f8e968   (module-relative pointer slot)
RealDataModel            0x1f8       (fake -> real)

Instance children        0x70        pointer to a container object
  container begin        +0x00       Instance* array, 8-byte stride
  container end          +0x08
Instance name            0x68        std::string*
Instance descriptor      0x18        ClassDescriptor* ; class name at +0x08 (std::string*)
Instance parent          0x60

BasePart primitive       0x178       Primitive*
Primitive position       0xd4        Vector3
Primitive rotation       0xb0        3x3
Primitive size           0x1c4       Vector3

DataModel workspace      0x168
Workspace camera         0x4a0
Camera rotation          0xc0        3x3
Camera position          0xe4        Vector3
Camera FOV               0x128       radians

Player model             0x2a0
Player team              0x2e0
Players local player     0x118

Humanoid health          0x184
Humanoid max health      0x1a0
Humanoid walkspeed       0x1c8
Humanoid jumppower       0x19c

Lighting brightness      0x100
Lighting fog end         0x114
```

Run `--scan` after each patch. A healthy chain prints `datamodel: 0x... valid=1`
followed by the service list; a stale one prints `valid=0` and names the block
to refresh. In game it also prints one `characters:` block per player with the
character address, humanoid health and head/root/torso positions, which is the
fastest way to see whether ESP has data to draw.

Notes on this build's shape (all verified with `tools/probe*.cpp` against
`version-4ea8e85481e34f46`):

- `Instance::Children` is a **pointer** at `+0x70` to a small container whose
  `+0x00` / `+0x08` are the begin / one-past-end of an 8-byte `Instance*` array
  (`ChildrenEnd = 0x8` in the dump is relative to that container). The reader
  keeps only entries whose `+0x60` parent points back at the owner.
- `Name` is not inline: `+0x68` holds a `std::string*`. Same for the class name
  at `descriptor + 0x08`. The reader tries pointer-to-`std::string` first, then
  an inline `std::string`, then raw `char*`.
- Camera is not a CFrame: 3x3 rotation at `0xc0` and a separate Vector3 at
  `0xe4`. The reader picks between row-major and transposed storage by testing
  `right x up == back`, so a flipped convention self-corrects. `FieldOfView` is
  stored in radians.


## layout

```
src/offsets.h    every version-dependent value
src/memory.*     RPM/WPM, module base, AOB scan, std::string reader
src/instance.*   Instance view: children, name, class, service resolution
src/game.*       camera, entity collection, world-to-screen
src/world.*      cached map geometry for the aimbot's visibility test
src/features.*   ESP, aimbot, memory writes, menu
src/render.*     Direct2D + DirectWrite primitives
src/ui.*         immediate-mode widgets
src/overlay.*    layered window, D3D11 swapchain, frame loop
src/main.cpp     attach + --scan
```

## notes

- External reads are not invisible: server-side movement/aim heuristics and
  client-side statistical flags still apply. Walkspeed and jumppower writes are
  the most visible thing in the build.
- "Visible only" tests each part's oriented box, because the actual mesh
  triangles are not readable at usable speed. Axis-aligned walls and floors are
  exact; a hollow or see-through decorative mesh can still hide a target that is
  really in plain sight, so turn the toggle off on maps that lean on imported
  meshes if it ever feels too strict. Parts within a few studs of your own head
  or the camera are ignored on purpose: that is where the first-person weapon
  viewmodel, own accessories and effects live, and they are drawn over the world
  rather than blocking it.
- The overlay window is `WS_EX_LAYERED` with `DwmExtendFrameIntoClientArea`.
  If it renders black, the DWM composition path is off (RDP sessions, some VMs).
