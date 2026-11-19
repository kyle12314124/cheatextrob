#include "render.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <map>
#include <utility>
#include <string>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;

namespace rnd {

namespace {

ComPtr<ID2D1Factory1>        g_factory;
ComPtr<ID2D1Device>          g_device;
ComPtr<ID2D1DeviceContext>   g_ctx;
ComPtr<ID2D1Bitmap1>         g_target;
ComPtr<ID2D1SolidColorBrush> g_brush;
ComPtr<IDWriteFactory>       g_dwrite;
ComPtr<IDXGISwapChain>       g_swap;
int g_width = 0;
int g_height = 0;

std::map<std::pair<int, bool>, IDWriteTextFormat*> g_formats;

D2D1_COLOR_F colorOf(uint32_t argb) {
    D2D1_COLOR_F c;
    c.a = ((argb >> 24) & 0xFF) / 255.0f;
    c.r = ((argb >> 16) & 0xFF) / 255.0f;
    c.g = ((argb >> 8) & 0xFF) / 255.0f;
    c.b = (argb & 0xFF) / 255.0f;
    return c;
}

bool bindTarget() {
    if (!g_swap || !g_ctx) return false;
    g_target.Reset();

    ComPtr<IDXGISurface> surface;
    if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;

    D2D1_BITMAP_PROPERTIES1 props{};
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    props.dpiX = 96.0f;
    props.dpiY = 96.0f;

    if (FAILED(g_ctx->CreateBitmapFromDxgiSurface(surface.Get(), &props, &g_target)))
        return false;

    g_ctx->SetTarget(g_target.Get());
    g_ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g_ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    return true;
}

IDWriteTextFormat* format(float size, bool bold) {
    int key = static_cast<int>(size + 0.5f);
    auto it = g_formats.find({key, bold});
    if (it != g_formats.end()) return it->second;

    IDWriteTextFormat* fmt = nullptr;
    if (FAILED(g_dwrite->CreateTextFormat(
            L"Segoe UI", nullptr,
            bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            static_cast<float>(key), L"en-us", &fmt)))
        return nullptr;

    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    g_formats[{key, bold}] = fmt;
    return fmt;
}

std::wstring widen(const char* s) {
    if (!s || !*s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
    out.resize(static_cast<size_t>(n - 1));
    return out;
}

} // namespace

bool init(ID3D11Device* dev, ID3D11DeviceContext* context, IDXGISwapChain* swap) {
    (void)context;
    g_swap = swap;

    D2D1_FACTORY_OPTIONS opts{};
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   __uuidof(ID2D1Factory1), &opts,
                                   reinterpret_cast<void**>(g_factory.GetAddressOf()));
    if (FAILED(hr)) return false;

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) return false;
    if (FAILED(g_factory->CreateDevice(dxgiDevice.Get(), &g_device))) return false;
    if (FAILED(g_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_ctx)))
        return false;

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(g_dwrite.GetAddressOf()))))
        return false;

    if (FAILED(g_ctx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &g_brush)))
        return false;

    return bindTarget();
}

void shutdown() {
    for (auto& [key, fmt] : g_formats)
        if (fmt) fmt->Release();
    g_formats.clear();
    g_brush.Reset();
    g_target.Reset();
    g_ctx.Reset();
    g_device.Reset();
    g_dwrite.Reset();
    g_factory.Reset();
    g_swap.Reset();
}

void begin(int width, int height) {
    if (!g_ctx) return;
    if (width != g_width || height != g_height || !g_target) {
        g_width = width;
        g_height = height;
        bindTarget();
    }
    g_ctx->BeginDraw();
    g_ctx->Clear(D2D1::ColorF(0, 0, 0, 0));
}

void end() {
    if (!g_ctx) return;
    HRESULT hr = g_ctx->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) bindTarget();
}

void releaseTarget() {
    if (g_ctx) g_ctx->SetTarget(nullptr);
    g_target.Reset();
    g_width = 0;
    g_height = 0;
}

void rect(float x, float y, float w, float h, uint32_t argb, float radius) {
    if (!g_ctx) return;
    g_brush->SetColor(colorOf(argb));
    D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + h);
    if (radius > 0.0f)
        g_ctx->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), g_brush.Get());
    else
        g_ctx->FillRectangle(r, g_brush.Get());
}

void rectGradient(float x, float y, float w, float h, uint32_t top, uint32_t bottom,
                  float radius, bool horizontal) {
    if (!g_ctx || w <= 0.0f || h <= 0.0f) return;

    D2D1_GRADIENT_STOP stops[2] = {{0.0f, colorOf(top)}, {1.0f, colorOf(bottom)}};
    ComPtr<ID2D1GradientStopCollection> collection;
    if (FAILED(g_ctx->CreateGradientStopCollection(stops, 2, &collection))) return;

    ComPtr<ID2D1LinearGradientBrush> brush;
    D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES props{};
    props.startPoint = horizontal ? D2D1::Point2F(x, y) : D2D1::Point2F(x, y);
    props.endPoint = horizontal ? D2D1::Point2F(x + w, y) : D2D1::Point2F(x, y + h);
    if (FAILED(g_ctx->CreateLinearGradientBrush(props, collection.Get(), &brush))) return;

    D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + h);
    if (radius > 0.0f)
        g_ctx->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.Get());
    else
        g_ctx->FillRectangle(r, brush.Get());
}

void rectOutline(float x, float y, float w, float h, uint32_t argb, float thickness, float radius) {
    if (!g_ctx) return;
    g_brush->SetColor(colorOf(argb));
    D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + h);
    if (radius > 0.0f)
        g_ctx->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), g_brush.Get(), thickness);
    else
        g_ctx->DrawRectangle(r, g_brush.Get(), thickness);
}

void line(float x1, float y1, float x2, float y2, uint32_t argb, float thickness) {
    if (!g_ctx) return;
    g_brush->SetColor(colorOf(argb));
    g_ctx->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), g_brush.Get(), thickness);
}

void circleFilled(float cx, float cy, float r, uint32_t argb) {
    if (!g_ctx) return;
    g_brush->SetColor(colorOf(argb));
    g_ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), g_brush.Get());
}

void circle(float cx, float cy, float r, uint32_t argb, float thickness) {
    if (!g_ctx) return;
    g_brush->SetColor(colorOf(argb));
    g_ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), g_brush.Get(), thickness);
}

void text(float x, float y, const char* s, uint32_t argb, float size, bool bold) {
    if (!g_ctx || !s || !*s) return;
    IDWriteTextFormat* fmt = format(size, bold);
    if (!fmt) return;
    std::wstring w = widen(s);
    if (w.empty()) return;

    // DrawTextLayout: the DrawText methods get macro-renamed by windows.h
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(g_dwrite->CreateTextLayout(w.c_str(), static_cast<UINT32>(w.size()), fmt,
                                          4000.0f, size * 2.0f, &layout)))
        return;
    g_brush->SetColor(colorOf(argb));
    g_ctx->DrawTextLayout(D2D1::Point2F(x, y), layout.Get(), g_brush.Get(),
                          D2D1_DRAW_TEXT_OPTIONS_NONE);
}

float textWidth(const char* s, float size, bool bold) {
    if (!g_ctx || !s || !*s) return 0.0f;
    IDWriteTextFormat* fmt = format(size, bold);
    if (!fmt) return 0.0f;
    std::wstring w = widen(s);
    if (w.empty()) return 0.0f;

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(g_dwrite->CreateTextLayout(w.c_str(), static_cast<UINT32>(w.size()), fmt,
                                          4000.0f, size * 2.0f, &layout)))
        return 0.0f;
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}

float textHeight(float size) { return size * 1.35f; }

void textClipped(float x, float y, float maxWidth, const char* s, uint32_t argb,
                 float size, bool bold) {
    if (!s) return;
    std::string cut(s);
    while (!cut.empty() && textWidth(cut.c_str(), size, bold) > maxWidth)
        cut.pop_back();
    text(x, y, cut.c_str(), argb, size, bold);
}

} // namespace rnd
