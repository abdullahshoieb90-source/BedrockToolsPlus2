#pragma once

#include <bedrocktoolsplus/sdk/Memory.hpp>
#include <bedrocktoolsplus/sdk/Offsets.hpp>
#include <bedrocktoolsplus/sdk/world/HitResult.hpp>

namespace bedrocktoolsplus::sdk {

class Level {
public:
    void* actorManager() const { return field<void*>(this, offsets::Level::mActorManager); }

    HitResult* storedHitResult() {
        return const_cast<HitResult*>(static_cast<const Level*>(this)->storedHitResult());
    }

    const HitResult* storedHitResult() const {
        // Level owns the HitResultWrapper through a Bedrock::UniqueOwnerPointer,
        // so the member holds { owner, value } and the wrapper has to be loaded
        // out of it. Treating the member as an embedded wrapper hands callers
        // unrelated Level memory as a HitResult, which is worse than returning
        // nothing: garbage block positions and entity pointers come out of it.
        const auto* owner = reinterpret_cast<const void*>(
            reinterpret_cast<std::uintptr_t>(this) + offsets::Level::mHitResultWrapper);
        const void* wrapper = field<void*>(owner, offsets::UniqueOwnerPointer::mValue);
        if (!wrapper || reinterpret_cast<std::uintptr_t>(wrapper) < 0x1000) return nullptr;

        return reinterpret_cast<const HitResult*>(
            reinterpret_cast<std::uintptr_t>(wrapper) + offsets::HitResultWrapper::mHitResult);
    }
};

}
