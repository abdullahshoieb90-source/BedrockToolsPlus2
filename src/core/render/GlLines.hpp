#pragma once

#include <cstddef>
#include <cstdint>

namespace bedrocktoolsplus::core::gllines {

// Everything the fallback renderer needs to place geometry in the world.
// The matrix is normally read back from the program the game is drawing with;
// these values are only the fallback used when that read fails.
struct Camera {
    float position[3] = {0.0f, 0.0f, 0.0f};
    float forward[3] = {0.0f, 0.0f, -1.0f};
    float fovDegrees = 70.0f;
};

// Draws a list of world-space line segments (pairs of xyz triples) with a
// self-contained GLES2 program. Used when the game's own tessellator cannot be
// reached, so an overlay still shows up on builds whose tessellator signatures
// no longer match. Returns false when OpenGL cannot be used at all.
//
// Every piece of state this touches is saved and restored, and any vertex
// array object the game may have bound is left alone, so the game's own
// rendering is not disturbed.
bool drawSegments(const Camera& camera, const float* vertices, std::size_t vertexCount, std::uint32_t color);

} // namespace bedrocktoolsplus::core::gllines
