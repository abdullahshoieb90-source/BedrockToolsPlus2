#include "core/render/RenderLevelHook.hpp"

#include <bedrocktoolsplus/memory/Signatures.hpp>
#include "core/memory/Hooks.hpp"

#include <mutex>
#include <vector>

namespace bedrocktoolsplus::core::renderlevel {

namespace {

std::mutex& registryMutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<Callback>& callbacks() {
    static std::vector<Callback> list;
    return list;
}

void (*s_original)(void*, void*, void*) = nullptr;
bool s_installed = false;

void detour(void* levelRenderer, void* screenContext, void* a3) {
    if (s_original) {
        s_original(levelRenderer, screenContext, a3);
    }

    // Held while the callbacks run: the list is only appended to, and from
    // module init, so contention is not a concern and no per-frame allocation
    // is needed to walk it safely.
    std::lock_guard<std::mutex> lock(registryMutex());
    for (auto* callback : callbacks()) {
        if (callback) callback(levelRenderer, screenContext, a3);
    }
}

} // namespace

void addCallback(Callback callback) {
    if (!callback) return;
    std::lock_guard<std::mutex> lock(registryMutex());
    callbacks().push_back(callback);
}

void install() {
    if (s_installed) return;

    const std::uintptr_t address = bedrocktoolsplus::memory::resolve(bedrocktoolsplus::memory::SignatureId::RenderLevel);
    if (!address) return;

    bedrocktoolsplus::hooks::install(reinterpret_cast<void*>(address),
                                     reinterpret_cast<void*>(&detour),
                                     reinterpret_cast<void**>(&s_original));
    s_installed = true;
}

bool installed() {
    return s_installed;
}

}
