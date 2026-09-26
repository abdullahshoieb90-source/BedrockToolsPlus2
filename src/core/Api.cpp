#include <bedrocktoolsplus/Api.hpp>
#include <bedrocktoolsplus/events/EventBus.hpp>
#include <bedrocktoolsplus/memory/Signatures.hpp>
#include "GameHooks.hpp"

namespace {
std::uintptr_t resolveSignature(std::uint16_t id) {
    if (id >= static_cast<std::uint16_t>(bedrocktoolsplus::memory::SignatureId::Count)) return 0;
    return bedrocktoolsplus::memory::resolve(static_cast<bedrocktoolsplus::memory::SignatureId>(id));
}

bedrocktoolsplus::sdk::ClientInstance* clientInstance() {
    return reinterpret_cast<bedrocktoolsplus::sdk::ClientInstance*>(bedrocktoolsplus::core::gamehooks::clientInstance());
}

std::uint64_t subscribe(bedrocktoolsplus::events::EventType type, bedrocktoolsplus::events::EventPriority priority, bedrocktoolsplus::api::EventCallback callback, void* userData) {
    if (!callback) return 0;
    return bedrocktoolsplus::events::bus().subscribeRaw(type, [type, callback, userData](void* payload) { callback(type, payload, userData); }, priority);
}

void unsubscribe(std::uint64_t subscription) {
    bedrocktoolsplus::events::bus().unsubscribe(subscription);
}

const bedrocktoolsplus::api::ApiV1 api{
    bedrocktoolsplus::api::AbiVersion,
    sizeof(bedrocktoolsplus::api::ApiV1),
    resolveSignature,
    clientInstance,
    subscribe,
    unsubscribe
};
}

extern "C" BEDROCKTOOLSPLUS_API const bedrocktoolsplus::api::ApiV1* BedrockToolsPlus_GetApi(std::uint32_t version) {
    return version == bedrocktoolsplus::api::AbiVersion ? &api : nullptr;
}
