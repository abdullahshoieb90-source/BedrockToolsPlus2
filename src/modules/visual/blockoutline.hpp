#pragma once

#include "../Module.hpp"
#include <cstdint>

// Draws a customizable outline around the block the crosshair is pointing at.
// The geometry is emitted from the same LevelRenderer::renderLevel detour the
// Hitbox/Chunk Border modules use, so it lives inside the game's own render
// pass and costs nothing when nothing is targeted.
class BlockOutlineModule : public Module {
public:
    BlockOutlineModule();
    ~BlockOutlineModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Colors are 0xAARRGGBB, the same packing the menu color picker uses.
    std::uint32_t outlineColor = 0xFFFFFFFF; // White
    std::uint32_t fillColor = 0xFFFFFFFF;    // White

    // 1.0 is the game's hairline; anything above is drawn as camera-facing
    // beams because mobile GLES drivers ignore GL line width. Stored in
    // hundredths of a block, matching the Hitbox module.
    float thickness = 1.0f;

    // Extra size around the block, in hundredths of a block (100 = one block).
    float expansion = 2.0f;

    bool fill = false;
    float fillOpacity = 0.25f;

    bool rgb = false;
    float rgbSpeed = 0.2f;

    // Diagnostic: appends every step of the render path to
    // blockoutline_trace.txt next to config.json.
    bool debugTrace = false;

private:
    void* m_tessBeginAddr = nullptr;
    void* m_tessColorAddr = nullptr;
    void* m_tessVertexAddr = nullptr;
    void* m_renderMaterialGroupAddr = nullptr;
};
