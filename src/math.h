#pragma once
#include <cmath>
#include <algorithm>

struct Vec3 {
    float x{}, y{}, z{};
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
inline Vec3 normalize(Vec3 a) {
    float l = length(a);
    return (l > 1e-6f) ? a * (1.0f / l) : Vec3{};
}

// Angle between two directions, degrees.
inline float angleDeg(Vec3 a, Vec3 b) {
    float d = dot(normalize(a), normalize(b));
    d = std::clamp(d, -1.0f, 1.0f);
    return std::acos(d) * (180.0f / 3.14159265358979323846f);
}
