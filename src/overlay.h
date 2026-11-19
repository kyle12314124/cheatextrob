#pragma once

namespace overlay {

// Blocks. Owns the window, the D3D11 device and the ImGui frame loop.
// Returns the process exit code when the overlay closes.
int run();

bool menuOpen();

} // namespace overlay
