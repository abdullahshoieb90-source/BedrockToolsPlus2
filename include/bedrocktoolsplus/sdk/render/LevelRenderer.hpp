#pragma once

#include <bedrocktoolsplus/sdk/Memory.hpp>
#include <bedrocktoolsplus/sdk/Offsets.hpp>
#include <bedrocktoolsplus/sdk/render/LevelRendererPlayer.hpp>

namespace bedrocktoolsplus::sdk {

class LevelRenderer {
public:
    void* renderChunkCoordinatorTable() {
        return reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(this) + offsets::LevelRenderer::mRenderChunkCoordinators);
    }

    LevelRendererPlayer* playerRenderer() { return field<LevelRendererPlayer*>(this, offsets::LevelRenderer::mLevelRendererPlayer); }
};

}
