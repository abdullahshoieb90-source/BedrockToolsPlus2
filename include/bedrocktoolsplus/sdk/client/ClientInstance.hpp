#pragma once

#include <bedrocktoolsplus/Api.hpp>
#include <bedrocktoolsplus/memory/Signatures.hpp>
#include <bedrocktoolsplus/sdk/Functions.hpp>
#include <bedrocktoolsplus/sdk/Memory.hpp>
#include <bedrocktoolsplus/sdk/Offsets.hpp>
#include <bedrocktoolsplus/sdk/render/LevelRenderer.hpp>
#include <bedrocktoolsplus/sdk/world/Actor.hpp>
#include <bedrocktoolsplus/sdk/world/BlockSource.hpp>

namespace bedrocktoolsplus::sdk {

class ClientInstance {
public:
    static ClientInstance* current() {
        const auto* runtime = api::find();
        return api::compatible(runtime) && runtime->clientInstance ? runtime->clientInstance() : nullptr;
    }

    BlockSource* region() { return virtualCall<BlockSource*>(this, offsets::VTable::ClientInstance_getRegion); }
    void* minecraftGame() { return virtualCall<void*>(this, offsets::VTable::ClientInstanceGetMinecraftGame); }
    LevelRenderer* levelRenderer() { return field<LevelRenderer*>(this, offsets::ClientInstance::mLevelRenderer); }

    Player* localPlayer(const api::ApiV1* runtime = nullptr) {
        using Function = Player*(*)(ClientInstance*);
        auto target = function<Function>(memory::SignatureId::ClientInstanceGetLocalPlayer, runtime);
        return target ? target(this) : nullptr;
    }

    void* packetSender(const api::ApiV1* runtime = nullptr) {
        using Function = void*(*)(ClientInstance*);
        auto target = function<Function>(memory::SignatureId::ClientInstanceGetPacketSender, runtime);
        return target ? target(this) : nullptr;
    }
};

}
