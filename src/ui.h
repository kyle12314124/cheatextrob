#pragma once
#include <cstdint>

// Minimal immediate-mode widgets on top of rnd.
// Every widget is addressed by the address of the value it edits.
namespace ui {

void beginFrame(float mouseX, float mouseY, bool mouseDown, bool mouseClicked);

void panelBegin(const char* title, float x, float y, float w, float h);
void panelEnd();
float panelX();
float panelY();

// Horizontal content layout: call columns(n) at the start of a tab, then
// nextColumn() to start the next one. Everything else flows downward.
void columns(int n);
void nextColumn();

bool tabBar(const void* id, const char* const* labels, int count, int& current);
bool checkbox(const void* id, const char* label, bool& value);
bool slider(const void* id, const char* label, float& value, float lo, float hi,
            const char* fmt = "%.1f");
bool cycle(const void* id, const char* label, int& value, const char* const* items, int count);
bool keybind(const void* id, const char* label, int& value);
bool swatches(const void* id, float color[4], const char* label = nullptr);
bool button(const void* id, const char* label, float width = 0.0f);
void sideStatus(const char* text, uint32_t color);
void label(const char* text);
void section(const char* text);
void dim(const char* text);
void separator();

} // namespace ui
