#pragma once
#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;

// Direct2D + DirectWrite on the D3D11 backbuffer.
// Colors are 0xAARRGGBB.
namespace rnd {

bool init(ID3D11Device* dev, ID3D11DeviceContext* context, IDXGISwapChain* swap);
void shutdown();

void begin(int width, int height);
void end();
void releaseTarget(); // call before ResizeBuffers: the target holds a ref

void rect(float x, float y, float w, float h, uint32_t argb, float radius = 0.0f);
void rectGradient(float x, float y, float w, float h, uint32_t top, uint32_t bottom,
                  float radius = 0.0f, bool horizontal = false);
void rectOutline(float x, float y, float w, float h, uint32_t argb,
                 float thickness = 1.0f, float radius = 0.0f);
void line(float x1, float y1, float x2, float y2, uint32_t argb, float thickness = 1.0f);
void circleFilled(float cx, float cy, float r, uint32_t argb);
void circle(float cx, float cy, float r, uint32_t argb, float thickness = 1.0f);

void text(float x, float y, const char* s, uint32_t argb, float size = 14.0f, bool bold = false);
float textWidth(const char* s, float size = 14.0f, bool bold = false);
float textHeight(float size = 14.0f);

// Text clipped to a rect (scrolling not needed, just guards the panel edges).
void textClipped(float x, float y, float maxWidth, const char* s, uint32_t argb,
                 float size = 14.0f, bool bold = false);

} // namespace rnd
