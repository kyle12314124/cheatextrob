#include "ui.h"
#include "render.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <functional>
#include <map>

namespace ui {

namespace {

// ---- palette ---------------------------------------------------------------
// Flat graphite, one accent, hairline rules. No gradients, no glow, no glass:
// the panel should read like a tool someone laid out by hand.
constexpr uint32_t kPanel    = 0xFF0E0F12;
constexpr uint32_t kSidebar  = 0xFF131519;
constexpr uint32_t kEdge     = 0xFF262A31;
constexpr uint32_t kDivider  = 0xFF1D2026;
constexpr uint32_t kText     = 0xFFE7E9ED;
constexpr uint32_t kTextOff  = 0xFF9BA1AC;
constexpr uint32_t kFaint    = 0xFF68707D;
constexpr uint32_t kAccent   = 0xFF5E9EFF;
constexpr uint32_t kTrack    = 0xFF1F2228;
constexpr uint32_t kChip     = 0xFF191C21;
constexpr uint32_t kKnob     = 0xFFECEEF2;

constexpr float kSideW  = 158.0f;
constexpr float kPadL   = 18.0f;   // content gutter, left and right
constexpr float kGutter = 20.0f;   // between columns
constexpr float kTop    = 26.0f;
constexpr float kRow    = 24.0f;
constexpr float kGap    = 2.0f;
constexpr float kRadius = 10.0f;
constexpr float kTabH   = 30.0f;

float g_mx = 0.0f, g_my = 0.0f;
bool  g_down = false;
bool  g_clicked = false;

float g_px = 64.0f, g_py = 34.0f, g_pw = 660.0f, g_ph = 330.0f;
float g_contentH = 0.0f;   // measured at the end of the previous frame
float g_cursor = 0.0f;
bool  g_placed = false;

const void* g_active = nullptr;    // slider holding the mouse
const void* g_capture = nullptr;   // keybind widget waiting for a key
bool  g_captureWait = false;       // ignore keys until the click is released
bool  g_dragPanel = false;
float g_dragOffX = 0.0f, g_dragOffY = 0.0f;

// column layout
int   g_colCount = 1;
int   g_col = 0;
float g_colW = 660.0f - kSideW - kPadL * 2.0f;
float g_colTop = 0.0f;
float g_colMax = 0.0f;

struct Key {
    const void* p;
    int slot;
};
bool operator<(const Key& a, const Key& b) {
    std::less<const void*> less;
    if (less(a.p, b.p)) return true;
    if (less(b.p, a.p)) return false;
    return a.slot < b.slot;
}
std::map<Key, float> g_anim;

bool inside(float x, float y, float w, float h) {
    return g_mx >= x && g_mx <= x + w && g_my >= y && g_my <= y + h;
}

float rowY() {
    float y = g_cursor;
    g_cursor += kRow + kGap;
    return y;
}

float colX() { return g_px + kSideW + kPadL + static_cast<float>(g_col) * (g_colW + kGutter); }
float colW() { return g_colW; }

float anim(const void* p, int slot, float target, float rate = 0.34f) {
    float& v = g_anim[Key{p, slot}];
    v += (target - v) * rate;
    if (std::fabs(target - v) < 0.002f) v = target;
    return v;
}

uint32_t mix(uint32_t a, uint32_t b, float t) {
    t = (t < 0.0f) ? 0.0f : (t > 1.0f ? 1.0f : t);
    auto ch = [&](int shift) {
        float ca = static_cast<float>((a >> shift) & 0xFF);
        float cb = static_cast<float>((b >> shift) & 0xFF);
        return static_cast<uint32_t>(ca + (cb - ca) * t) << shift;
    };
    return (ch(24) | ch(16) | ch(8) | ch(0));
}

void rule(float x, float y, float w, uint32_t col = 0x10FFFFFF) {
    if (w > 0.0f) rnd::rect(x, y, w, 1.0f, col);
}

} // namespace

void beginFrame(float mouseX, float mouseY, bool mouseDown, bool mouseClicked) {
    g_mx = mouseX;
    g_my = mouseY;
    g_down = mouseDown;
    g_clicked = mouseClicked;
    if (!g_down) g_active = nullptr;
}

float panelX() { return g_px; }
float panelY() { return g_py; }

void panelBegin(const char* title, float x, float y, float w, float h) {
    if (!g_placed) {
        g_px = x;
        g_py = y;
        g_placed = true;
    }
    g_pw = w;

    // Landscape card: the height follows the last measured layout, clamped to
    // the caller's maximum.
    g_ph = (g_contentH > 0.0f) ? std::clamp(g_contentH, 252.0f, h) : h;

    if (g_px < -g_pw + 100.0f) g_px = -g_pw + 100.0f;
    if (g_py < 0.0f) g_py = 0.0f;

    if (g_clicked && inside(g_px, g_py, g_pw, 40.0f)) {
        g_dragPanel = true;
        g_dragOffX = g_mx - g_px;
        g_dragOffY = g_my - g_py;
    }
    if (!g_down) g_dragPanel = false;
    if (g_dragPanel && g_down) {
        g_px = g_mx - g_dragOffX;
        g_py = g_my - g_dragOffY;
        if (g_px < -g_pw + 100.0f) g_px = -g_pw + 100.0f;
        if (g_py < 0.0f) g_py = 0.0f;
    }

    // depth: two flat offsets
    rnd::rect(g_px + 1.0f, g_py + 5.0f, g_pw, g_ph, 0x18000000, kRadius + 2.0f);
    rnd::rect(g_px, g_py, g_pw, g_ph, kPanel, kRadius);

    // sidebar, square on its right edge
    rnd::rect(g_px, g_py, kSideW, g_ph, kSidebar, kRadius);
    rnd::rect(g_px + kSideW * 0.5f, g_py, kSideW * 0.5f, g_ph, kSidebar);
    rnd::rect(g_px + kSideW, g_py, 1.0f, g_ph, kDivider);

    rnd::rectOutline(g_px + 0.5f, g_py + 0.5f, g_pw - 1.0f, g_ph - 1.0f, kEdge, 1.0f, kRadius);

    // brand block
    rnd::rect(g_px + 16.0f, g_py + 16.0f, 3.0f, 16.0f, kAccent, 1.5f);
    rnd::text(g_px + 28.0f, g_py + 14.0f, title, kText, 14.5f, true);
    rnd::text(g_px + 28.0f, g_py + 33.0f, "external overlay", kFaint, 9.5f);

    // footer: state + menu key, pinned to the bottom of the rail
    {
        float fy = g_py + g_ph - 30.0f;
        float cw = rnd::textWidth("F1", 9.5f) + 14.0f;
        rnd::rect(g_px + kSideW - 14.0f - cw, fy, cw, 16.0f, kChip, 4.0f);
        rnd::rectOutline(g_px + kSideW - 13.5f - cw, fy + 0.5f, cw - 1.0f, 15.0f, kEdge, 1.0f, 4.0f);
        rnd::text(g_px + kSideW - 14.0f - cw + 7.0f, fy + 2.5f, "F1", kFaint, 9.5f);
    }

    g_col = 0;
    g_colCount = 1;
    g_colW = g_pw - kSideW - kPadL * 2.0f;
    g_colTop = g_py + kTop;
    g_cursor = g_colTop;
    g_colMax = 0.0f;
}

void panelEnd() {
    g_colMax = std::max(g_colMax, g_cursor);
    g_contentH = g_colMax - g_py + 16.0f;
}

void columns(int n) {
    g_colCount = std::max(1, n);
    g_colW = (g_pw - kSideW - kPadL * 2.0f - kGutter * static_cast<float>(g_colCount - 1)) /
             static_cast<float>(g_colCount);
    g_col = 0;
    g_cursor = g_colTop;
    g_colMax = 0.0f;
}

void nextColumn() {
    g_colMax = std::max(g_colMax, g_cursor);
    if (g_col + 1 < g_colCount) ++g_col;
    g_cursor = g_colTop;
}

bool tabBar(const void* id, const char* const* labels, int count, int& current) {
    float x = g_px + 10.0f;
    float w = kSideW - 20.0f;
    float y = g_py + 58.0f;
    bool changed = false;

    for (int i = 0; i < count; ++i) {
        bool hover = inside(x, y, w, kTabH);
        bool active = (i == current);
        float t = anim(id, 0x100 + i, active ? 1.0f : (hover ? 0.5f : 0.0f), 0.3f);

        if (t > 0.01f)
            rnd::rect(x, y, w, kTabH, mix(0x00000000, 0x14FFFFFF, t), 6.0f);
        if (active) {
            rnd::rect(x, y + 8.0f, 2.5f, kTabH - 16.0f, kAccent, 1.25f);
        }

        rnd::text(x + 14.0f, y + 8.0f, labels[i],
                  mix(kTextOff, kText, active ? 1.0f : t * 0.7f), 12.5f,
                  active);

        if (hover && g_clicked && !active) {
            current = i;
            changed = true;
        }
        y += kTabH + 2.0f;
    }
    return changed;
}

bool checkbox(const void* id, const char* label, bool& value) {
    float y = rowY();
    float x = colX();
    float w = colW();
    bool hover = inside(x - 8.0f, y, w + 16.0f, kRow);

    float ht = anim(id, 0, hover ? 1.0f : 0.0f, 0.3f);
    float t = anim(id, 1, value ? 1.0f : 0.0f, 0.32f);

    if (ht > 0.01f)
        rnd::rect(x - 8.0f, y, w + 16.0f, kRow, mix(0x00FFFFFF, 0x0CFFFFFF, ht), 5.0f);

    rnd::text(x, y + 4.0f, label, mix(kTextOff, kText, value ? 1.0f : ht * 0.55f), 13.0f);

    const float sw = 32.0f, sh = 17.0f;
    float sx = x + w - sw;
    float sy = y + (kRow - sh) * 0.5f;
    rnd::rect(sx, sy, sw, sh, mix(0xFF23262C, kAccent, t), sh * 0.5f);

    const float kr = sh * 0.5f - 2.5f;
    rnd::circleFilled(sx + sh * 0.5f + t * (sw - sh), sy + sh * 0.5f, kr, kKnob);

    if (hover && g_clicked) {
        value = !value;
        return true;
    }
    return false;
}

bool slider(const void* id, const char* label, float& value, float lo, float hi,
            const char* fmt) {
    float y = rowY();
    float x = colX();
    float w = colW();

    char valueBuf[48];
    std::snprintf(valueBuf, sizeof(valueBuf), fmt, value);

    rnd::text(x, y + 1.0f, label, kTextOff, 12.5f);
    float vw = rnd::textWidth(valueBuf, 12.0f, true);
    rnd::text(x + w - vw, y + 1.0f, valueBuf, kText, 12.0f, true);

    float ty = y + 21.0f;
    float th = 4.0f;
    bool hover = inside(x, ty - 7.0f, w, th + 14.0f);
    float ht = anim(id, 2, hover ? 1.0f : 0.0f, 0.3f);

    if (hover && g_clicked) g_active = id;
    if (g_active == id && g_down) {
        float t = (g_mx - x) / w;
        t = (t < 0.0f) ? 0.0f : (t > 1.0f ? 1.0f : t);
        value = lo + t * (hi - lo);
    }

    float frac = (hi > lo) ? (value - lo) / (hi - lo) : 0.0f;
    frac = (frac < 0.0f) ? 0.0f : (frac > 1.0f ? 1.0f : frac);

    rnd::rect(x, ty, w, th, mix(kTrack, 0xFF2A2E36, ht), th * 0.5f);
    if (frac > 0.001f)
        rnd::rect(x, ty, w * frac, th, kAccent, th * 0.5f);

    float kx = x + w * frac;
    float kr = 6.0f + ht * 0.5f;
    rnd::circleFilled(kx, ty + th * 0.5f, kr + 1.0f, 0x30000000);
    rnd::circleFilled(kx, ty + th * 0.5f, kr, kKnob);

    g_cursor += 15.0f;
    return false;
}

bool cycle(const void* id, const char* label, int& value, const char* const* items, int count) {
    float y = rowY();
    float x = colX();
    float w = colW();
    bool hover = inside(x - 8.0f, y, w + 16.0f, kRow);
    float ht = anim(id, 0, hover ? 1.0f : 0.0f, 0.3f);

    rnd::text(x, y + 4.0f, label, kTextOff, 12.5f);

    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s", (value >= 0 && value < count) ? items[value] : "-");
    float bw = rnd::textWidth(buf, 12.0f) + 20.0f;
    float bx = x + w - bw;

    rnd::rect(bx, y + 2.0f, bw, 20.0f, mix(kChip, 0xFF20242B, ht), 5.0f);
    rnd::rectOutline(bx + 0.5f, y + 2.5f, bw - 1.0f, 19.0f,
                     mix(kEdge, 0xFF3A404A, ht), 1.0f, 5.0f);
    rnd::text(bx + 10.0f, y + 5.0f, buf, mix(kText, 0xFFFFFFFF, ht), 12.0f);

    if (hover && g_clicked) {
        value = (value + 1) % (count > 0 ? count : 1);
        return true;
    }
    return false;
}

namespace {
void keyName(int vk, char* buf, size_t n) {
    switch (vk) {
        case 0:            std::snprintf(buf, n, "none");   return;
        case VK_LBUTTON:   std::snprintf(buf, n, "left");   return;
        case VK_RBUTTON:   std::snprintf(buf, n, "right");  return;
        case VK_MBUTTON:   std::snprintf(buf, n, "middle"); return;
        case VK_XBUTTON1:  std::snprintf(buf, n, "mouse4"); return;
        case VK_XBUTTON2:  std::snprintf(buf, n, "mouse5"); return;
        case VK_SHIFT:     std::snprintf(buf, n, "shift");  return;
        case VK_CONTROL:   std::snprintf(buf, n, "ctrl");   return;
        case VK_MENU:      std::snprintf(buf, n, "alt");    return;
        case VK_SPACE:     std::snprintf(buf, n, "space");  return;
        case VK_TAB:       std::snprintf(buf, n, "tab");    return;
        case VK_CAPITAL:   std::snprintf(buf, n, "caps");   return;
        case VK_INSERT:    std::snprintf(buf, n, "insert"); return;
        case VK_DELETE:    std::snprintf(buf, n, "delete"); return;
        case VK_HOME:      std::snprintf(buf, n, "home");   return;
        case VK_END:       std::snprintf(buf, n, "end");    return;
        case VK_PRIOR:     std::snprintf(buf, n, "pgup");   return;
        case VK_NEXT:      std::snprintf(buf, n, "pgdn");   return;
        case VK_UP:        std::snprintf(buf, n, "up");     return;
        case VK_DOWN:      std::snprintf(buf, n, "down");   return;
        case VK_LEFT:      std::snprintf(buf, n, "left");   return;
        case VK_RIGHT:     std::snprintf(buf, n, "right");  return;
        default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F24) { std::snprintf(buf, n, "f%d", vk - VK_F1 + 1); return; }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        std::snprintf(buf, n, "%c", static_cast<char>(vk));
        return;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        std::snprintf(buf, n, "num%d", vk - VK_NUMPAD0);
        return;
    }
    std::snprintf(buf, n, "0x%02X", vk);
}
} // namespace

// Click to arm, then press the key or mouse button you want. F1 (the menu key)
// and the left button are ignored so opening the bind can never eat the menu
// hotkey; Escape cancels.
bool keybind(const void* id, const char* label, int& value) {
    float y = rowY();
    float x = colX();
    float w = colW();
    bool hover = inside(x - 8.0f, y, w + 16.0f, kRow);
    float ht = anim(id, 0, hover ? 1.0f : 0.0f, 0.3f);
    bool changed = false;

    rnd::text(x, y + 4.0f, label, kTextOff, 12.5f);

    const bool capturing = (g_capture == id);
    char buf[48];
    if (capturing) std::snprintf(buf, sizeof(buf), "press key...");
    else keyName(value, buf, sizeof(buf));

    float bw = std::max(88.0f, rnd::textWidth(buf, 12.0f) + 20.0f);
    float bx = x + w - bw;

    rnd::rect(bx, y + 2.0f, bw, 20.0f, mix(kChip, 0xFF20242B, ht), 5.0f);
    rnd::rectOutline(bx + 0.5f, y + 2.5f, bw - 1.0f, 19.0f,
                     capturing ? kAccent : mix(kEdge, 0xFF3A404A, ht), 1.0f, 5.0f);
    rnd::text(bx + 10.0f, y + 5.0f, buf,
              capturing ? kAccent : mix(kText, 0xFFFFFFFF, ht), 12.0f);

    if (hover && g_clicked) {
        g_capture = capturing ? nullptr : id;
        g_captureWait = true;
    }

    if (g_capture == id) {
        if (g_captureWait) {
            // Let the click that armed the bind go before listening, so the
            // mouse button never becomes the bind itself.
            if (!g_down) g_captureWait = false;
        } else {
            for (int vk = 1; vk < 256; ++vk) {
                if (vk == VK_LBUTTON || vk == VK_F1) continue;
                if (!(GetAsyncKeyState(vk) & 0x8000)) continue;
                if (vk == VK_ESCAPE) { g_capture = nullptr; break; }
                if (vk == VK_LSHIFT || vk == VK_RSHIFT) vk = VK_SHIFT;
                else if (vk == VK_LCONTROL || vk == VK_RCONTROL) vk = VK_CONTROL;
                else if (vk == VK_LMENU || vk == VK_RMENU) vk = VK_MENU;
                value = vk;
                g_capture = nullptr;
                changed = true;
                break;
            }
        }
    }
    return changed;
}

bool swatches(const void* id, float color[4], const char* label) {    static const uint32_t presets[8] = {
        0xFFFF4D4F, 0xFFFF9F1C, 0xFFFFD60A, 0xFF32D74B,
        0xFF40A9FF, 0xFF3DD6D0, 0xFFB57BFF, 0xFFE9EAEE,
    };

    float y = rowY();
    float x = colX();
    float w = colW();
    bool changed = false;
    const float r = 7.0f;
    const float step = 20.0f;
    const float span = step * 7.0f + r * 2.0f;
    const float startX = x + w - span + r;

    if (label) rnd::text(x, y + 3.0f, label, kTextOff, 11.5f);

    uint32_t packed = (static_cast<uint32_t>(color[0] * 255.0f) << 16) |
                      (static_cast<uint32_t>(color[1] * 255.0f) << 8) |
                      static_cast<uint32_t>(color[2] * 255.0f);

    for (int i = 0; i < 8; ++i) {
        float cx = startX + i * step;
        float cy = y + r + 2.0f;
        uint32_t c = presets[i];
        bool hover = inside(cx - r - 2.0f, cy - r - 2.0f, r * 2.0f + 4.0f, r * 2.0f + 4.0f);
        bool selected = ((packed & 0xFFFFFF) == (c & 0xFFFFFF));

        if (selected)
            rnd::circle(cx, cy, r + 3.0f, kText, 1.5f);
        else if (hover)
            rnd::circle(cx, cy, r + 2.0f, 0x66FFFFFF, 1.0f);
        rnd::circleFilled(cx, cy, r, c);

        if (hover && g_clicked) {
            color[0] = ((c >> 16) & 0xFF) / 255.0f;
            color[1] = ((c >> 8) & 0xFF) / 255.0f;
            color[2] = (c & 0xFF) / 255.0f;
            color[3] = 1.0f;
            changed = true;
        }
    }
    (void)id;
    return changed;
}

bool button(const void* id, const char* label, float width) {
    float y = rowY();
    float x = colX();
    float w = (width > 0.0f) ? width : colW();
    float h = 28.0f;
    bool hover = inside(x, y, w, h);
    float ht = anim(id, 0, hover ? 1.0f : 0.0f, 0.3f);

    rnd::rect(x, y, w, h, mix(kChip, 0xFF20242B, ht), 6.0f);
    rnd::rectOutline(x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f,
                     mix(kEdge, 0xFF3A404A, ht), 1.0f, 6.0f);
    float tw = rnd::textWidth(label, 12.5f);
    rnd::text(x + (w - tw) * 0.5f, y + 7.0f, label, mix(kTextOff, kText, ht), 12.5f);

    if (hover && g_clicked) return true;
    return false;
}

void label(const char* text) {
    float y = rowY();
    rnd::text(colX(), y + 4.0f, text, kText, 13.0f);
}

void sideStatus(const char* text, uint32_t color) {
    float fy = g_py + g_ph - 30.0f;
    rnd::circleFilled(g_px + 17.0f, fy + 8.0f, 3.0f, color);
    rnd::text(g_px + 27.0f, fy + 2.0f, text, kFaint, 10.0f);
}

void section(const char* text) {
    g_cursor += 10.0f;

    char upper[64];
    size_t n = 0;
    for (; text[n] && n < sizeof(upper) - 1; ++n)
        upper[n] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[n])));
    upper[n] = '\0';

    float y = g_cursor;
    float x = colX();
    float w = colW();
    float tw = rnd::textWidth(upper, 9.5f, true);

    rnd::text(x, y, upper, kFaint, 9.5f, true);
    rule(x + tw + 8.0f, y + 5.0f, w - tw - 8.0f);

    g_cursor += 21.0f;
}

void dim(const char* text) {
    float y = rowY();
    rnd::text(colX(), y + 3.0f, text, kFaint, 11.0f);
}

void separator() {
    g_cursor += 7.0f;
    rule(colX(), g_cursor, colW());
    g_cursor += 13.0f;
}

} // namespace ui
