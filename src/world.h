#pragma once
#include "math.h"
#include <cstddef>
#include <cstdint>
#include <vector>

// Cached world occluders for the aimbot's "visible only" test.
//
// A full Workspace walk in a live game measured ~15 000 parts in ~140 ms, so the
// scan runs incrementally in the background: a couple of hundred nodes per
// frame to discover parts, and a rolling refresh of the cached transforms. That
// keeps the cost well under a millisecond per frame.
namespace world {

void reset();
void update(bool active);                        // call once per frame

// Character models to keep out of the cache. Games may parent characters in a
// folder and many FPS games replace the Humanoid with an AnimationController, so
// the rig cannot be recognised by class alone - the caller already knows every
// player's character address, which is exact.
void setCharacters(const std::vector<uintptr_t>& models);

bool segmentBlocked(Vec3 from, Vec3 to);         // solid geometry between two points
bool firstBlocker(Vec3 from, Vec3 to, Vec3* center, Vec3* ext, float* t,
                  uintptr_t* part = nullptr);
void buildNow();                                 // blocking build, for --scan

size_t occluders();                              // cached occluder count
size_t partsSeen();                              // parts discovered by the walk
bool ready();                                    // walk finished at least once

} // namespace world
