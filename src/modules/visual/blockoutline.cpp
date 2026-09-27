#include "blockoutline.hpp"

#include <bedrocktoolsplus/memory/Signatures.hpp>
#include "core/render/GlLines.hpp"
#include "core/render/RenderLevelHook.hpp"
#include "config/ConfigManager.hpp"
#include <bedrocktoolsplus/events/EventBus.hpp>
#include <bedrocktoolsplus/sdk/Memory.hpp>
#include <bedrocktoolsplus/sdk/Offsets.hpp>
#include <bedrocktoolsplus/sdk/Types.hpp>
#include <bedrocktoolsplus/sdk/world/HitResult.hpp>
#include <bedrocktoolsplus/sdk/world/Level.hpp>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

using Vec3 = bedrocktoolsplus::sdk::Vec3;
using AABB = bedrocktoolsplus::sdk::AABB;
using BlockPos = bedrocktoolsplus::sdk::BlockPos;
using HitResult = bedrocktoolsplus::sdk::HitResult;

using Line = std::pair<Vec3, Vec3>;

// Material primitive modes (see the material config reference:
// None, QuadList, TriangleList, TriangleStrip, LineList, Line).
constexpr int kPrimitiveQuadList = 1;
constexpr int kPrimitiveLineList = 4;

// HitResult::mType values.
constexpr int kHitResultBlock = 0;

// The fill sits exactly on the block surface, so push it out by a hair to
// keep it from z-fighting with the block face it covers.
constexpr float kFillInset = 0.002f;

// Sanity bounds for the resolved target. kMaxReach is far beyond any real
// reach, it only rejects nonsense; kHitSlack lets the hit point sit a hair
// outside the block face and still count as belonging to it.
constexpr float kMaxReach = 128.0f;
constexpr float kHitSlack = 0.05f;

// Fallback ray: a player's eye is 1.62 blocks above their feet, and nothing
// is targeted beyond the reach, so the walk stops there.
constexpr float kEyeHeight = 1.62f;
constexpr float kRayReach = 6.0f;

typedef void (*Tessellator_begin_t)(void* tessellator, void* debugCallback, int primitiveMode, int vertexCount, int noIndices);
typedef void (*Tessellator_color_t)(void* tessellator, float r, float g, float b, float a);
typedef void (*Tessellator_vertex_t)(void* tessellator, float x, float y, float z);
typedef void (*MeshHelpers_renderMeshImmediately_t)(void* screenContext, void* tessellator, void* material, char* pad);

// Level::getHitResult() lets the game itself resolve the hit result, which is
// safer than walking Level's UniqueOwnerPointer by hand.
typedef void* (*Level_getHitResult_t)(void* level);

// Used by the fallback ray, which walks the blocks along the crosshair when
// the hit result cannot be trusted.
typedef void* (*BlockSource_getBlock_t)(void* region, const BlockPos& pos);
typedef bool (*BlockSource_isSolidBlockingBlock_t)(void* region, const BlockPos& pos);

struct HashedString {
    std::uint64_t mStrHash;
    std::string mStr;
    mutable const HashedString* mLastMatch;

    HashedString() : mStrHash(0), mStr(), mLastMatch(nullptr) {}

    explicit HashedString(const char* str) : mLastMatch(nullptr) {
        mStr = str ? str : "";
        mStrHash = computeHash(mStr);
    }

private:
    static std::uint64_t computeHash(const std::string& str) {
        if (str.empty()) return 0;
        constexpr std::uint64_t kOffset = 0xCBF29CE484222325ULL;
        constexpr std::uint64_t kPrime = 0x100000001B3ULL;
        std::uint64_t hash = kOffset;
        for (char ch : str)
            hash = static_cast<std::uint64_t>(static_cast<unsigned char>(ch)) ^ (kPrime * hash);
        return hash;
    }
};

struct MaterialPtr {
    void* sharedPtrData[2]{nullptr, nullptr};

    MaterialPtr() = default;
    MaterialPtr(const MaterialPtr&) = delete;
    MaterialPtr& operator=(const MaterialPtr&) = delete;

    MaterialPtr(MaterialPtr&& other) noexcept
        : sharedPtrData{other.sharedPtrData[0], other.sharedPtrData[1]} {
        other.sharedPtrData[0] = nullptr;
        other.sharedPtrData[1] = nullptr;
    }

    MaterialPtr& operator=(MaterialPtr&& other) noexcept {
        if (this != &other) {
            sharedPtrData[0] = other.sharedPtrData[0];
            sharedPtrData[1] = other.sharedPtrData[1];
            other.sharedPtrData[0] = nullptr;
            other.sharedPtrData[1] = nullptr;
        }
        return *this;
    }

    ~MaterialPtr() {}

    explicit operator bool() const {
        return sharedPtrData[0] != nullptr;
    }
};

static std::uintptr_t resolveADRP(std::uint32_t* insns, std::size_t count, std::uint32_t targetReg) {
    for (std::size_t i = 0; i < count; i++) {
        std::uint32_t insn = insns[i];
        if ((insn & 0x1F) != targetReg) continue;

        if ((insn & 0x9F000000) == 0x90000000) {
            std::uintptr_t page = ((std::uintptr_t)&insns[i] & ~0xFFFULL)
                                + ((std::int64_t)((std::uint64_t)((insn >> 3) & 0x1FFFFC | (insn >> 29) & 3) << 43) >> 31);

            for (std::size_t j = i + 1; j < count; j++) {
                std::uint32_t add = insns[j];
                if ((add & 0xFF000000) == 0x91000000 &&
                    ((add >> 5) & 0x1F) == targetReg &&
                    (add & 0x1F) == targetReg) {
                    std::uint32_t imm12 = (add >> 10) & 0xFFF;
                    if (add & 0x400000) imm12 <<= 12;
                    return page + imm12;
                }
                if ((add & 0x1F) == targetReg) break;
            }
        }
        if ((insn & 0x9F000000) == 0x10000000) {
            std::int64_t imm = (std::int64_t)((std::uint64_t)((insn >> 3) & 0x1FFFFC | (insn >> 29)) << 43) >> 43;
            return (std::uintptr_t)&insns[i] + imm;
        }
    }
    return 0;
}

static BlockOutlineModule* g_blockOutlineMod = nullptr;

static Tessellator_begin_t                 s_tessBegin = nullptr;
static Tessellator_color_t                 s_tessColor = nullptr;
static Tessellator_vertex_t                s_tessVertex = nullptr;
static MeshHelpers_renderMeshImmediately_t s_renderMesh = nullptr;
static Level_getHitResult_t s_getHitResult = nullptr;
static BlockSource_getBlock_t s_getBlock = nullptr;
static BlockSource_isSolidBlockingBlock_t s_isSolidBlockingBlock = nullptr;

static MaterialPtr s_matSelection;
static MaterialPtr s_matFill;
static std::uintptr_t s_renderMaterialGroup = 0;


static std::string traceFilePath() {
    static const std::string path = [] {
        std::string configPath = bedrocktoolsplus::config::ConfigManager::get().getConfigPath();
        const std::size_t slash = configPath.find_last_of("/\\");
        const std::string dir = slash == std::string::npos ? std::string() : configPath.substr(0, slash + 1);
        return dir + "blockoutline_trace.txt";
    }();
    return path;
}

static void* g_localPlayer = nullptr;
static std::chrono::steady_clock::time_point g_lastTick{};

// After leaving a world the cached player pointer is freed, and the render
// hook would keep dereferencing it, so remember when it was last seen alive.

// Temporary diagnostic: when Debug Trace is on, every step of the render path
// is appended to blockoutline_trace.txt next to config.json. The file is
// flushed after every line, so the last marker survives a crash and shows
// exactly how far the module got.
static void traceReset() {
    std::string path = traceFilePath();
    if (path.empty()) return;
    if (FILE* f = std::fopen(path.c_str(), "w")) std::fclose(f);
}

// The hook writes a marker every frame, so the file is restarted once it grows
// past a size that is still comfortable to open on a phone.
constexpr long kMaxTraceBytes = 256 * 1024;

static void trace(const char* text) {
    if (!g_blockOutlineMod || !g_blockOutlineMod->debugTrace) return;

    std::string path = traceFilePath();
    if (path.empty()) return;

    if (FILE* probe = std::fopen(path.c_str(), "a+")) {
        std::fseek(probe, 0, SEEK_END);
        const long size = std::ftell(probe);
        std::fclose(probe);
        if (size > kMaxTraceBytes) {
            if (FILE* fresh = std::fopen(path.c_str(), "w")) {
                std::fprintf(fresh, "... restarted\n");
                std::fclose(fresh);
            }
        }
    }

    if (FILE* f = std::fopen(path.c_str(), "a")) {
        std::fprintf(f, "%s\n", text);
        std::fclose(f);
    }
}

static void traceFormat(const char* format, ...) {
    if (!g_blockOutlineMod || !g_blockOutlineMod->debugTrace) return;

    char buffer[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    trace(buffer);
}

static void s_blockOutlineTickCallback(void* player) {
    g_localPlayer = player;
    g_lastTick = std::chrono::steady_clock::now();

    // Signatures are matched after the modules are constructed, so a toggle
    // that happened before that left the render hook uninstalled. Retrying
    // here, on the game thread, installs it as soon as it becomes possible.
    if (g_blockOutlineMod && g_blockOutlineMod->enabled && !bedrocktoolsplus::core::renderlevel::installed()) {
        bedrocktoolsplus::core::renderlevel::install();
        traceFormat("tick installed=%d", (int)bedrocktoolsplus::core::renderlevel::installed());
    }
}

static bool localPlayerFresh() {
    if (!g_localPlayer) return false;
    const auto elapsed = std::chrono::steady_clock::now() - g_lastTick;
    return std::chrono::duration<float>(elapsed).count() <= 2.0f;
}

static bool finite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

static MaterialPtr getMaterial(const char* name) {
    if (!s_renderMaterialGroup) return {};

    HashedString hs(name);

    void** vtable = *reinterpret_cast<void***>(s_renderMaterialGroup);
    if (!vtable || !vtable[bedrocktoolsplus::sdk::offsets::VTable::RenderMaterialGroup_getMaterial]) return {};

    using getMat_t = MaterialPtr(*)(void*, const HashedString*);
    return reinterpret_cast<getMat_t>(vtable[bedrocktoolsplus::sdk::offsets::VTable::RenderMaterialGroup_getMaterial])((void*)s_renderMaterialGroup, &hs);
}

static void ensureMaterials(bool needBeam) {
    if (!s_renderMaterialGroup) return;

    // The block highlight material: depth tested and alpha blended, which is
    // exactly what both the hairline pass and the translucent fill want.
    if (!s_matSelection) s_matSelection = getMaterial("selection_box");

    // Thick edges are drawn as filled quads, where the highlight material
    // washes the color out. Prefer a plain vertex-color fill instead, and only
    // look one up once the module actually draws beams.
    if (!needBeam || s_matFill) return;

    static const char* kFillNames[] = {
        "ui_fill_color",
        "ui_textured_and_glcolor",
        "debug_filled_box",
        "selection_box"
    };
    for (const char* name : kFillNames) {
        s_matFill = getMaterial(name);
        if (s_matFill) break;
    }
}

static std::vector<Line> boxEdges(const AABB& box) {
    const Vec3 mn = box.min;
    const Vec3 mx = box.max;

    std::vector<Line> edges;
    edges.reserve(12);

    // Bottom and top face.
    edges.push_back({{mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}});
    edges.push_back({{mx.x, mn.y, mn.z}, {mx.x, mn.y, mx.z}});
    edges.push_back({{mx.x, mn.y, mx.z}, {mn.x, mn.y, mx.z}});
    edges.push_back({{mn.x, mn.y, mx.z}, {mn.x, mn.y, mn.z}});

    edges.push_back({{mn.x, mx.y, mn.z}, {mx.x, mx.y, mn.z}});
    edges.push_back({{mx.x, mx.y, mn.z}, {mx.x, mx.y, mx.z}});
    edges.push_back({{mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}});
    edges.push_back({{mn.x, mx.y, mx.z}, {mn.x, mx.y, mn.z}});

    // Verticals.
    edges.push_back({{mn.x, mn.y, mn.z}, {mn.x, mx.y, mn.z}});
    edges.push_back({{mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}});
    edges.push_back({{mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}});
    edges.push_back({{mn.x, mn.y, mx.z}, {mn.x, mx.y, mx.z}});

    return edges;
}

static void drawLines(void* tessellator, void* screenContext,
                      void* matLine, void* matBeam,
                      const std::vector<Line>& lines,
                      std::uint32_t color,
                      float camX, float camY, float camZ,
                      float halfWidth, bool thick) {
    if (lines.empty()) return;

    const float r = ((color >> 16) & 0xFF) / 255.0f;
    const float g = ((color >>  8) & 0xFF) / 255.0f;
    const float b = ((color      ) & 0xFF) / 255.0f;
    const float a = ((color >> 24) & 0xFF) / 255.0f;

    char pad[0x58];

    // Thick pass: every edge becomes a camera-facing quad, so the apparent
    // width follows the thickness setting from any angle. Mobile GLES drivers
    // ignore GL line width, so this is the only way to get a fat outline.
    if (thick && matBeam) {
        s_tessBegin(tessellator, nullptr, kPrimitiveQuadList, static_cast<int>(lines.size() * 8), 0);
        s_tessColor(tessellator, r, g, b, a);

        for (const auto& line : lines) {
            Vec3 p1 = line.first;
            Vec3 p2 = line.second;
            p1.x -= camX; p1.y -= camY; p1.z -= camZ;
            p2.x -= camX; p2.y -= camY; p2.z -= camZ;

            float dx = p2.x - p1.x;
            float dy = p2.y - p1.y;
            float dz = p2.z - p1.z;
            const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (len < 1e-5f) continue;
            dx /= len; dy /= len; dz /= len;

            // The camera sits at the origin of this relative space, so the
            // vector to the edge midpoint is the view direction.
            const float mx = (p1.x + p2.x) * 0.5f;
            const float my = (p1.y + p2.y) * 0.5f;
            const float mz = (p1.z + p2.z) * 0.5f;

            // side = dir x view: perpendicular to both the edge and the eye
            // ray, which makes the quad face the player from any angle.
            float sx = dy * mz - dz * my;
            float sy = dz * mx - dx * mz;
            float sz = dx * my - dy * mx;
            float sLen = std::sqrt(sx * sx + sy * sy + sz * sz);
            if (sLen < 1e-5f) {
                // Looking straight down the edge: any perpendicular works.
                if (std::fabs(dy) < 0.9f) { sx = -dz; sy = 0.0f; sz = dx; }
                else                      { sx = 1.0f; sy = 0.0f; sz = 0.0f; }
                sLen = std::sqrt(sx * sx + sy * sy + sz * sz);
                if (sLen < 1e-5f) continue;
            }
            sx = sx / sLen * halfWidth;
            sy = sy / sLen * halfWidth;
            sz = sz / sLen * halfWidth;

            // Overshoot both ends by half the width so corners stay solid.
            const float ex = dx * halfWidth;
            const float ey = dy * halfWidth;
            const float ez = dz * halfWidth;

            const Vec3 quad[4] = {
                {p1.x - ex - sx, p1.y - ey - sy, p1.z - ez - sz},
                {p2.x + ex - sx, p2.y + ey - sy, p2.z + ez - sz},
                {p2.x + ex + sx, p2.y + ey + sy, p2.z + ez + sz},
                {p1.x - ex + sx, p1.y - ey + sy, p1.z - ez + sz}
            };

            // Both windings, so back-face culling never eats an edge.
            for (int i = 0; i < 4; ++i)
                s_tessVertex(tessellator, quad[i].x, quad[i].y, quad[i].z);
            for (int i = 3; i >= 0; --i)
                s_tessVertex(tessellator, quad[i].x, quad[i].y, quad[i].z);
        }

        std::memset(pad, 0, sizeof(pad));
        s_renderMesh(screenContext, tessellator, matBeam, pad);
    }

    // Hairline pass: keeps the edge crisp even when the quads shrink below a
    // pixel at long range, and is all that runs at thickness 1.
    if (matLine) {
        s_tessBegin(tessellator, nullptr, kPrimitiveLineList, static_cast<int>(lines.size() * 2), 0);
        s_tessColor(tessellator, r, g, b, a);

        for (const auto& line : lines) {
            Vec3 p1 = line.first;
            Vec3 p2 = line.second;
            p1.x -= camX; p1.y -= camY; p1.z -= camZ;
            p2.x -= camX; p2.y -= camY; p2.z -= camZ;
            s_tessVertex(tessellator, p1.x, p1.y, p1.z);
            s_tessVertex(tessellator, p2.x, p2.y, p2.z);
        }

        std::memset(pad, 0, sizeof(pad));
        s_renderMesh(screenContext, tessellator, matLine, pad);
    }
}

static void drawFaces(void* tessellator, void* screenContext, void* material,
                      const AABB& box, std::uint32_t color, float alpha,
                      float camX, float camY, float camZ) {
    if (!material || alpha <= 0.0f) return;

    const float r = ((color >> 16) & 0xFF) / 255.0f;
    const float g = ((color >>  8) & 0xFF) / 255.0f;
    const float b = ((color      ) & 0xFF) / 255.0f;
    const float a = alpha > 1.0f ? 1.0f : alpha;

    const Vec3 mn = box.min;
    const Vec3 mx = box.max;

    // The eight corners of the box, shifted into camera-relative space.
    auto at = [&](bool hx, bool hy, bool hz) {
        return Vec3{(hx ? mx.x : mn.x) - camX, (hy ? mx.y : mn.y) - camY, (hz ? mx.z : mn.z) - camZ};
    };

    const Vec3 c000 = at(false, false, false);
    const Vec3 c100 = at(true,  false, false);
    const Vec3 c110 = at(true,  true,  false);
    const Vec3 c010 = at(false, true,  false);
    const Vec3 c001 = at(false, false, true);
    const Vec3 c101 = at(true,  false, true);
    const Vec3 c111 = at(true,  true,  true);
    const Vec3 c011 = at(false, true,  true);

    // Four corners per face, wound around the face.
    const Vec3 faces[6][4] = {
        {c000, c001, c101, c100}, // -Y
        {c010, c110, c111, c011}, // +Y
        {c000, c100, c110, c010}, // -Z
        {c001, c011, c111, c101}, // +Z
        {c000, c010, c011, c001}, // -X
        {c100, c101, c111, c110}  // +X
    };

    s_tessBegin(tessellator, nullptr, kPrimitiveQuadList, 6 * 8, 0);
    s_tessColor(tessellator, r, g, b, a);

    for (const auto& face : faces) {
        // Both windings, so the fill reads from inside and outside the box.
        for (int i = 0; i < 4; ++i)
            s_tessVertex(tessellator, face[i].x, face[i].y, face[i].z);
        for (int i = 3; i >= 0; --i)
            s_tessVertex(tessellator, face[i].x, face[i].y, face[i].z);
    }

    char pad[0x58];
    std::memset(pad, 0, sizeof(pad));
    s_renderMesh(screenContext, tessellator, material, pad);
}

// Validates one candidate hit result and reports the block it points at.
// Every field is range checked, so a wrong offset can only make the frame
// skip, never hand garbage coordinates to the renderer.
static bool hitResultTarget(const HitResult* hit, BlockPos& out) {
    if (!hit || hit->type() != kHitResultBlock) return false;

    const Vec3& hitPos = hit->position();
    const Vec3& eye = hit->startPosition();
    if (!finite(hitPos) || !finite(eye)) return false;

    // The shot that produced this hit result has to be a short one.
    const float dx = hitPos.x - eye.x;
    const float dy = hitPos.y - eye.y;
    const float dz = hitPos.z - eye.z;
    if (!(dx * dx + dy * dy + dz * dz <= kMaxReach * kMaxReach)) return false; // NaN safe

    // The hit point lies on a face of the block, so it has to fall inside the
    // block's own bounds.
    auto hitLiesOn = [&](const BlockPos& pos) {
        return hitPos.x >= (float)pos.x - kHitSlack && hitPos.x <= (float)pos.x + 1.0f + kHitSlack &&
               hitPos.y >= (float)pos.y - kHitSlack && hitPos.y <= (float)pos.y + 1.0f + kHitSlack &&
               hitPos.z >= (float)pos.z - kHitSlack && hitPos.z <= (float)pos.z + 1.0f + kHitSlack;
    };

    BlockPos target = hit->blockPosition();
    if (hitLiesOn(target)) {
        out = target;
        return true;
    }

    // The hit position sits exactly on a block face, so stepping a hair along
    // the ray lands inside the block that was hit.
    const Vec3& dir = hit->rayDirection();
    if (!finite(dir)) return false;

    const BlockPos fallback{
        (int)std::floor(hitPos.x + dir.x * 0.001f),
        (int)std::floor(hitPos.y + dir.y * 0.001f),
        (int)std::floor(hitPos.z + dir.z * 0.001f)
    };
    if (!hitLiesOn(fallback)) return false;

    out = fallback;
    return true;
}

// Eye position and look direction of the local player, taken from the actor's
// own components rather than from the camera, so the ray is the one the game
// aims with (in third person the camera sits behind the player).
static bool playerEyeAndLook(void* player, Vec3& eyeOut, Vec3& dirOut) {
    if (!player) return false;

    const std::uintptr_t actor = reinterpret_cast<std::uintptr_t>(player);
    const std::uintptr_t svc = *(std::uintptr_t*)(actor + bedrocktoolsplus::sdk::offsets::Actor::mStateVectorComponent);
    const std::uintptr_t rot = *(std::uintptr_t*)(actor + bedrocktoolsplus::sdk::offsets::Actor::mActorRotationComponent);
    if (!svc || svc < 0x1000 || !rot || rot < 0x1000) return false;

    const Vec3 pos = *(Vec3*)svc;
    const float pitch = *(float*)(rot + 0);
    const float yaw = *(float*)(rot + 4);
    if (!finite(pos) || !std::isfinite(pitch) || !std::isfinite(yaw)) return false;

    eyeOut = Vec3{pos.x, pos.y + kEyeHeight, pos.z};

    // Minecraft yaw: 0 faces +Z and grows toward -X. Pitch grows downward.
    const float yawRad = yaw * (3.14159265358979323846f / 180.0f);
    const float pitchRad = pitch * (3.14159265358979323846f / 180.0f);
    const float horizontal = std::cos(pitchRad);

    dirOut = Vec3{-std::sin(yawRad) * horizontal, -std::sin(pitchRad), std::cos(yawRad) * horizontal};
    return finite(dirOut);
}

// Walks the blocks along the crosshair and reports the first solid one. Only
// used when the hit result cannot be read, so the cheap exact path stays the
// one that normally runs.
static bool raycastTarget(void* player, BlockPos& out) {
    if (!s_getBlock || !s_isSolidBlockingBlock || !player) return false;

    const std::uintptr_t dimension = *(std::uintptr_t*)((std::uintptr_t)player + bedrocktoolsplus::sdk::offsets::Actor::mDimension);
    if (!dimension || dimension < 0x1000) return false;

    const std::uintptr_t blockSource = *(std::uintptr_t*)(dimension + bedrocktoolsplus::sdk::offsets::Dimension::mBlockSource);
    if (!blockSource || blockSource < 0x1000) return false;
    void* region = (void*)blockSource;

    Vec3 eye{};
    Vec3 dir{};
    if (!playerEyeAndLook(player, eye, dir)) return false;

    // Amanatides & Woo: step from block face to block face along the ray.
    int x = (int)std::floor(eye.x);
    int y = (int)std::floor(eye.y);
    int z = (int)std::floor(eye.z);

    const int stepX = dir.x > 0.0f ? 1 : -1;
    const int stepY = dir.y > 0.0f ? 1 : -1;
    const int stepZ = dir.z > 0.0f ? 1 : -1;

    const float inf = std::numeric_limits<float>::infinity();
    const float deltaX = dir.x == 0.0f ? inf : std::fabs(1.0f / dir.x);
    const float deltaY = dir.y == 0.0f ? inf : std::fabs(1.0f / dir.y);
    const float deltaZ = dir.z == 0.0f ? inf : std::fabs(1.0f / dir.z);

    float maxX = deltaX * (stepX > 0 ? ((float)x + 1.0f - eye.x) : (eye.x - (float)x));
    float maxY = deltaY * (stepY > 0 ? ((float)y + 1.0f - eye.y) : (eye.y - (float)y));
    float maxZ = deltaZ * (stepZ > 0 ? ((float)z + 1.0f - eye.z) : (eye.z - (float)z));

    float travelled = 0.0f;

    for (int guard = 0; guard < 64 && travelled <= kRayReach; ++guard) {
        // The block the eye is inside is never the target.
        if (travelled > 0.0f) {
            const BlockPos candidate{x, y, z};
            if (s_getBlock(region, candidate) && s_isSolidBlockingBlock(region, candidate)) {
                out = candidate;
                return true;
            }
        }

        if (maxX < maxY && maxX < maxZ) {
            travelled = maxX; x += stepX; maxX += deltaX;
        } else if (maxY < maxZ) {
            travelled = maxY; y += stepY; maxY += deltaY;
        } else {
            travelled = maxZ; z += stepZ; maxZ += deltaZ;
        }
    }

    return false;
}

// Resolves the block the crosshair is on and returns its (expanded) box.
//
// No engine call is made from inside the render hook: the target comes from
// Level's stored hit result, read through the SDK owner pointer, and only if
// that fails to validate is Level::getHitResult() asked instead.
static bool resolveTargetBox(void* localPlayer, float expansionBlocks, AABB& out) {
    const std::uintptr_t levelPtr = *(std::uintptr_t*)((std::uintptr_t)localPlayer + bedrocktoolsplus::sdk::offsets::Actor::mLevel);
    if (!levelPtr || levelPtr < 0x1000) { trace("target:no-level"); return false; }

    auto* level = reinterpret_cast<bedrocktoolsplus::sdk::Level*>(levelPtr);

    BlockPos target{0, 0, 0};
    bool found = false;

    const HitResult* stored = level->storedHitResult();
    found = hitResultTarget(stored, target);
    traceFormat("target:stored hit=%p found=%d", (void*)stored, (int)found);

    if (!found && s_getHitResult) {
        HitResult* asked = reinterpret_cast<HitResult*>(s_getHitResult(reinterpret_cast<void*>(levelPtr)));
        found = hitResultTarget(asked, target);
        traceFormat("target:gethit hit=%p found=%d", (void*)asked, (int)found);
    }

    if (!found) {
        found = raycastTarget(localPlayer, target);
        traceFormat("target:ray found=%d pos=%d %d %d", (int)found, target.x, target.y, target.z);
    }

    if (!found) return false;

    out.min = Vec3{(float)target.x - expansionBlocks, (float)target.y - expansionBlocks, (float)target.z - expansionBlocks};
    out.max = Vec3{(float)target.x + 1.0f + expansionBlocks,
                   (float)target.y + 1.0f + expansionBlocks,
                   (float)target.z + 1.0f + expansionBlocks};
    return true;
}

static std::uint32_t hsvToColor(float hue) {
    if (!(hue >= 0.0f)) hue = 0.0f; // NaN guard
    hue -= std::floor(hue);

    float r = std::fabs(hue * 6.0f - 3.0f) - 1.0f;
    float g = 2.0f - std::fabs(hue * 6.0f - 2.0f);
    float b = 2.0f - std::fabs(hue * 6.0f - 4.0f);

    r = std::clamp(r, 0.0f, 1.0f);
    g = std::clamp(g, 0.0f, 1.0f);
    b = std::clamp(b, 0.0f, 1.0f);

    return 0xFF000000u
         | (static_cast<std::uint32_t>(r * 255.0f + 0.5f) << 16)
         | (static_cast<std::uint32_t>(g * 255.0f + 0.5f) << 8)
         | (static_cast<std::uint32_t>(b * 255.0f + 0.5f));
}

static std::uint32_t rainbowColor(float speed) {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    const float seconds = std::chrono::duration<float>(clock::now() - start).count();
    return hsvToColor(seconds * speed * 0.5f);
}

// Temporary diagnostic: which signatures the game build could be matched
// against. If the tessellator ones are 0 while unrelated ones are not, the
// patterns are stale for this build and the OpenGL fallback has to carry the
// drawing.
static void traceSignatureProbe() {
    using bedrocktoolsplus::memory::SignatureId;
    traceFormat("sigdump RenderLevel=%p Begin=%p Color=%p Vertex=%p Mesh1=%p Mesh2=%p Group=%p Fov=%p HitResult=%p GetBlock=%p",
                (void*)bedrocktoolsplus::memory::resolve(SignatureId::RenderLevel),
                (void*)bedrocktoolsplus::memory::resolve(SignatureId::TessellatorBegin),
                (void*)bedrocktoolsplus::memory::resolve(SignatureId::TessellatorColor),
                (void*)bedrocktoolsplus::memory::resolve(SignatureId::TessellatorVertex),
                (void*)bedrocktoolsplus::memory::resolve(SignatureId::MeshHelpersRenderMeshImmediately),
                (void*)bedrocktoolsplus::memory::resolve(SignatureId::MeshHelpersRenderMeshImmediately2),
                (void*)bedrocktoolsplus::memory::resolve(SignatureId::RenderMaterialGroupCommon),
                (void*)bedrocktoolsplus::memory::resolve(SignatureId::GetFov),
                (void*)bedrocktoolsplus::memory::resolve(SignatureId::LevelGetHitResult),
                (void*)bedrocktoolsplus::memory::resolve(SignatureId::BlockSourceGetBlock));
}

static bool tessellatorReady() {
    return s_tessBegin && s_tessColor && s_tessVertex && s_renderMesh;
}

// The draw path runs every frame, so a missing signature would otherwise fill
// the trace with the same line thousands of times.
static bool traceThrottled() {
    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<float>(now - last).count() < 2.0f) return false;
    last = now;
    return true;
}

// Fallback draw: plain OpenGL lines in world space, used when the game's
// tessellator cannot be reached on this build.
static void drawLinesWithGl(const std::vector<Line>& lines, std::uint32_t color,
                            float camX, float camY, float camZ) {
    if (lines.empty()) return;

    std::vector<float> vertices;
    vertices.reserve(lines.size() * 6);
    for (const auto& line : lines) {
        vertices.push_back(line.first.x);
        vertices.push_back(line.first.y);
        vertices.push_back(line.first.z);
        vertices.push_back(line.second.x);
        vertices.push_back(line.second.y);
        vertices.push_back(line.second.z);
    }

    bedrocktoolsplus::core::gllines::Camera camera{};
    camera.position[0] = camX;
    camera.position[1] = camY;
    camera.position[2] = camZ;

    Vec3 eye{};
    Vec3 direction{};
    if (playerEyeAndLook(g_localPlayer, eye, direction)) {
        camera.forward[0] = direction.x;
        camera.forward[1] = direction.y;
        camera.forward[2] = direction.z;
    }

    const bool drawn = bedrocktoolsplus::core::gllines::drawSegments(camera, vertices.data(), vertices.size() / 3, color);
    traceFormat("hook:gl-draw ok=%d lines=%d", (int)drawn, (int)lines.size());
}

static void s_blockOutlineRender(void* _this, void* screenContext, void* a3) {
    auto* mod = g_blockOutlineMod;
    if (!mod || !mod->enabled) return;
    trace("hook:enter");
    if (!_this || (std::uintptr_t)_this < 0x1000) { trace("hook:no-this"); return; }
    if (!localPlayerFresh()) { trace("hook:stale-player"); return; }
    if (!screenContext || (std::uintptr_t)screenContext < 0x1000) { trace("hook:no-screen"); return; }

    static bool probedOnce = false;
    if (!probedOnce) { probedOnce = true; traceSignatureProbe(); }

    // Tessellator signatures are matched once per build; when they fail there
    // is no game-side way to draw, so fall back to OpenGL instead of stopping.
    if (!tessellatorReady() && traceThrottled()) {
        traceFormat("hook:no-tess begin=%p color=%p vertex=%p mesh=%p",
                    s_tessBegin, s_tessColor, s_tessVertex, s_renderMesh);
    }

    const std::uintptr_t lrpPtr = *(std::uintptr_t*)((std::uintptr_t)_this + bedrocktoolsplus::sdk::offsets::LevelRenderer::mLevelRendererPlayer);
    if (!lrpPtr || lrpPtr < 0x1000) { trace("hook:no-lrp"); return; }

    std::uintptr_t tessellatorPtr = 0;
    if (tessellatorReady()) {
        tessellatorPtr = *(std::uintptr_t*)((std::uintptr_t)screenContext + bedrocktoolsplus::sdk::offsets::ScreenContext::mTessellator);
        if (!tessellatorPtr || tessellatorPtr < 0x1000) {
            tessellatorPtr = 0;
            if (traceThrottled()) trace("hook:no-tess-ptr");
        }
    }
    void* tessellator = (void*)tessellatorPtr;

    const float camX = *(float*)(lrpPtr + bedrocktoolsplus::sdk::offsets::LevelRendererPlayer::mCamPos);
    const float camY = *(float*)(lrpPtr + bedrocktoolsplus::sdk::offsets::LevelRendererPlayer::mCamPos + 4);
    const float camZ = *(float*)(lrpPtr + bedrocktoolsplus::sdk::offsets::LevelRendererPlayer::mCamPos + 8);

    const float expansion = std::clamp(mod->expansion, 0.0f, 100.0f) * 0.01f;
    AABB box{};
    if (!resolveTargetBox(g_localPlayer, expansion, box)) { trace("hook:no-target"); return; }
    traceFormat("hook:target %.2f %.2f %.2f -> %.2f %.2f %.2f",
                box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z);

    const float thickness = std::clamp(mod->thickness, 1.0f, 20.0f);
    const bool thick = thickness > 1.05f;

    trace("hook:materials");
    if (tessellator) ensureMaterials(thick);
    traceFormat("hook:state tess=%p/%p/%p mesh=%p group=%p lrp=%p",
                s_tessBegin, s_tessColor, s_tessVertex, s_renderMesh,
                (void*)s_renderMaterialGroup, (void*)lrpPtr);

    void* overlayMaterial = (void*)(lrpPtr + bedrocktoolsplus::sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial);
    void* matLine = s_matSelection ? (void*)&s_matSelection : overlayMaterial;
    void* matBeam = s_matFill ? (void*)&s_matFill : matLine;

    std::uintptr_t colorHolderPtr = 0;
    if (tessellator) {
        colorHolderPtr = *(std::uintptr_t*)((std::uintptr_t)screenContext + bedrocktoolsplus::sdk::offsets::ScreenContext::mColorHolder);
        if (!colorHolderPtr || colorHolderPtr < 0x1000) {
            colorHolderPtr = 0;
            if (traceThrottled()) trace("hook:no-colorholder");
        }
    }
    float* colorHolder = (float*)colorHolderPtr;

    float savedColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    if (colorHolder) {
        savedColor[0] = colorHolder[0];
        savedColor[1] = colorHolder[1];
        savedColor[2] = colorHolder[2];
        savedColor[3] = colorHolder[3];
        colorHolder[0] = 1.0f;
        colorHolder[1] = 1.0f;
        colorHolder[2] = 1.0f;
        colorHolder[3] = 1.0f;
    }

    if (mod->fill && tessellator) {
        AABB fillBox = box;
        fillBox.min.x -= kFillInset; fillBox.min.y -= kFillInset; fillBox.min.z -= kFillInset;
        fillBox.max.x += kFillInset; fillBox.max.y += kFillInset; fillBox.max.z += kFillInset;
        drawFaces(tessellator, screenContext, matLine, fillBox, mod->fillColor, mod->fillOpacity, camX, camY, camZ);
    }

    const std::uint32_t outlineColor = mod->rgb ? rainbowColor(mod->rgbSpeed) : mod->outlineColor;
    const std::vector<Line> edges = boxEdges(box);

    trace("hook:draw");
    if (tessellator) {
        drawLines(tessellator, screenContext, matLine, matBeam, edges, outlineColor,
                  camX, camY, camZ, thickness * 0.01f * 0.5f, thick);
    } else {
        drawLinesWithGl(edges, outlineColor, camX, camY, camZ);
    }

    // While tracing, a magenta box is drawn three blocks ahead of the eye no
    // matter what the crosshair is on: if it shows up the draw path is alive
    // and any missing outline is a targeting problem, not a render problem.
    if (mod->debugTrace) {
        Vec3 eye{};
        Vec3 dir{};
        if (playerEyeAndLook(g_localPlayer, eye, dir)) {
            const BlockPos ahead{(int)std::floor(eye.x + dir.x * 3.0f),
                                 (int)std::floor(eye.y + dir.y * 3.0f),
                                 (int)std::floor(eye.z + dir.z * 3.0f)};
            const AABB debugBox{Vec3{(float)ahead.x, (float)ahead.y, (float)ahead.z},
                                Vec3{(float)ahead.x + 1.0f, (float)ahead.y + 1.0f, (float)ahead.z + 1.0f}};
            if (tessellator) {
                drawLines(tessellator, screenContext, matLine, matLine, boxEdges(debugBox),
                          0xFFFF00FFu, camX, camY, camZ, 0.0f, false);
            } else {
                drawLinesWithGl(boxEdges(debugBox), 0xFFFF00FFu, camX, camY, camZ);
            }
            traceFormat("hook:debug-box cam=%.1f %.1f %.1f box=%d %d %d",
                        camX, camY, camZ, ahead.x, ahead.y, ahead.z);
        } else {
            trace("hook:debug-box-no-eye");
        }
    }

    trace("hook:done");

    if (colorHolder) {
        colorHolder[0] = savedColor[0];
        colorHolder[1] = savedColor[1];
        colorHolder[2] = savedColor[2];
        colorHolder[3] = savedColor[3];
    }
}

} // namespace

BlockOutlineModule::BlockOutlineModule()
    : Module("Block Outline", "Draws a custom outline around the block you are looking at.") {
    showInMenu = true;
    g_blockOutlineMod = this;
}

BlockOutlineModule::~BlockOutlineModule() {
    if (g_blockOutlineMod == this) g_blockOutlineMod = nullptr;
}

void BlockOutlineModule::onInit() {
    const std::uintptr_t tb = bedrocktoolsplus::memory::resolve(bedrocktoolsplus::memory::SignatureId::TessellatorBegin);
    if (tb) { m_tessBeginAddr = (void*)tb; s_tessBegin = (Tessellator_begin_t)tb; }

    const std::uintptr_t tc = bedrocktoolsplus::memory::resolve(bedrocktoolsplus::memory::SignatureId::TessellatorColor);
    if (tc) { m_tessColorAddr = (void*)tc; s_tessColor = (Tessellator_color_t)tc; }

    const std::uintptr_t tv = bedrocktoolsplus::memory::resolve(bedrocktoolsplus::memory::SignatureId::TessellatorVertex);
    if (tv) { m_tessVertexAddr = (void*)tv; s_tessVertex = (Tessellator_vertex_t)tv; }

    const std::uintptr_t rm = bedrocktoolsplus::memory::resolve(bedrocktoolsplus::memory::SignatureId::MeshHelpersRenderMeshImmediately2);
    if (rm) {
        s_renderMesh = (MeshHelpers_renderMeshImmediately_t)rm;
    } else {
        const std::uintptr_t rm5 = bedrocktoolsplus::memory::resolve(bedrocktoolsplus::memory::SignatureId::MeshHelpersRenderMeshImmediately);
        if (rm5) s_renderMesh = (MeshHelpers_renderMeshImmediately_t)rm5;
    }

    const std::uintptr_t rmg = bedrocktoolsplus::memory::resolve(bedrocktoolsplus::memory::SignatureId::RenderMaterialGroupCommon);
    if (rmg) {
        m_renderMaterialGroupAddr = (void*)rmg;
        const std::uintptr_t groupAddr = resolveADRP(reinterpret_cast<std::uint32_t*>(rmg), 2, 0);
        if (groupAddr) {
            s_renderMaterialGroup = groupAddr + bedrocktoolsplus::sdk::offsets::MaterialGroup::mRenderMaterialGroupOffset;
        }
    }

    const std::uintptr_t ghr = bedrocktoolsplus::memory::resolve(bedrocktoolsplus::memory::SignatureId::LevelGetHitResult);
    if (ghr) s_getHitResult = (Level_getHitResult_t)ghr;

    const std::uintptr_t gb = bedrocktoolsplus::memory::resolve(bedrocktoolsplus::memory::SignatureId::BlockSourceGetBlock);
    if (gb) s_getBlock = (BlockSource_getBlock_t)gb;

    const std::uintptr_t sb = bedrocktoolsplus::memory::resolve(bedrocktoolsplus::memory::SignatureId::BlockSourceIsSolidBlockingBlock);
    if (sb) s_isSolidBlockingBlock = (BlockSource_isSolidBlockingBlock_t)sb;

    bedrocktoolsplus::core::renderlevel::addCallback(&s_blockOutlineRender);

    bedrocktoolsplus::events::bus().subscribe<bedrocktoolsplus::events::LocalPlayerTickEvent>([](auto& event) {
        s_blockOutlineTickCallback(event.player);
    });

    traceFormat("init tess=%p/%p/%p mesh=%p group=%p hitresult=%p block=%p solid=%p",
                s_tessBegin, s_tessColor, s_tessVertex, s_renderMesh,
                (void*)s_renderMaterialGroup, s_getHitResult,
                (void*)s_getBlock, (void*)s_isSolidBlockingBlock);
}

void BlockOutlineModule::onEnable() {
    bedrocktoolsplus::core::renderlevel::install();
    traceFormat("enable installed=%d", (int)bedrocktoolsplus::core::renderlevel::installed());
}

void BlockOutlineModule::onDisable() {
}

void BlockOutlineModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    thickness = j.value("thickness", thickness);
    expansion = j.value("expansion", expansion);
    fill = j.value("fill", fill);
    fillOpacity = j.value("fillOpacity", fillOpacity);
    rgb = j.value("rgb", rgb);
    rgbSpeed = j.value("rgbSpeed", rgbSpeed);

    const bool wasTracing = debugTrace;
    debugTrace = j.value("debugTrace", debugTrace);
    if (debugTrace && !wasTracing) traceReset();

    // The menu picker can hand back #RRGGBB (alpha 0); treat that as opaque
    // so the outline never silently disappears.
    auto parseColor = [&](const std::string& key, std::uint32_t& outColor) {
        if (!j.contains(key)) return;
        const std::string hexStr = j[key].get<std::string>();
        if (hexStr.empty() || hexStr[0] != '#') return;
        try {
            std::uint32_t parsed = std::stoul(hexStr.substr(1), nullptr, 16);
            if ((parsed & 0xFF000000u) == 0) parsed |= 0xFF000000u;
            outColor = parsed;
        } catch (...) {}
    };

    parseColor("outlineColor", outlineColor);
    parseColor("fillColor", fillColor);

    thickness = std::clamp(thickness, 1.0f, 20.0f);
    expansion = std::clamp(expansion, 0.0f, 100.0f);
    fillOpacity = std::clamp(fillOpacity, 0.0f, 1.0f);
    rgbSpeed = std::clamp(rgbSpeed, 0.05f, 1.0f);
}

void BlockOutlineModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    j["thickness"] = thickness;
    j["expansion"] = expansion;
    j["fill"] = fill;
    j["fillOpacity"] = fillOpacity;
    j["rgb"] = rgb;
    j["rgbSpeed"] = rgbSpeed;
    j["debugTrace"] = debugTrace;

    char hexOutline[12];
    char hexFill[12];
    std::snprintf(hexOutline, sizeof(hexOutline), "#%08X", outlineColor);
    std::snprintf(hexFill, sizeof(hexFill), "#%08X", fillColor);

    j["outlineColor"] = std::string(hexOutline);
    j["fillColor"] = std::string(hexFill);
}
