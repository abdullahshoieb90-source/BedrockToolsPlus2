#include "forceglobalrp.hpp"
#include <bedrocktoolsplus/memory/Signatures.hpp>
#include <bedrocktoolsplus/sdk/Memory.hpp>
#include <bedrocktoolsplus/sdk/network/Packet.hpp>
#include "core/memory/Hooks.hpp"

static ForceGlobalRPModule* g_forceGlobalRPMod = nullptr;

static void (*original_ResourcePackStackPacket_handle)(void* _this, void* a1, void* a2, std::shared_ptr<bedrocktoolsplus::sdk::Packet>& packet) = nullptr;
static void (*original_ResourcePacksInfoPacket_handle)(void* _this, void* a1, void* a2, std::shared_ptr<bedrocktoolsplus::sdk::Packet>& packet) = nullptr;
static void ResourcePacksInfoPacket_handle_hook(void* _this, void* a1, void* a2, std::shared_ptr<bedrocktoolsplus::sdk::Packet>& packet) {
    if (g_forceGlobalRPMod && g_forceGlobalRPMod->enabled && packet.get() != nullptr) {
        bedrocktoolsplus::sdk::Packet* pkt = packet.get();
        bedrocktoolsplus::sdk::field<bool>(pkt, bedrocktoolsplus::sdk::offsets::ResourcePacksInfoPacket::mResourcePackRequired) = false;
        bedrocktoolsplus::sdk::field<bool>(pkt, bedrocktoolsplus::sdk::offsets::ResourcePacksInfoPacket::mForceDisableVibrantVisuals) = false;
    }
    
    if (original_ResourcePacksInfoPacket_handle) {
        original_ResourcePacksInfoPacket_handle(_this, a1, a2, packet);
    }
}
static void ResourcePackStackPacket_handle_hook(void* _this, void* a1, void* a2, std::shared_ptr<bedrocktoolsplus::sdk::Packet>& packet) {
    if (g_forceGlobalRPMod && g_forceGlobalRPMod->enabled && packet.get() != nullptr) {
        bedrocktoolsplus::sdk::Packet* pkt = packet.get();
        bedrocktoolsplus::sdk::field<bool>(pkt, bedrocktoolsplus::sdk::offsets::ResourcePackStackPacket::mResourcePackRequired) = false;
    }
    
    if (original_ResourcePacksInfoPacket_handle) {
        original_ResourcePackStackPacket_handle(_this, a1, a2, packet);
    }
}

void ForceGlobalRPModule::initFunc1() {
    if (m_func1Target) return;
    
    uintptr_t addr = bedrocktoolsplus::memory::resolve(bedrocktoolsplus::memory::SignatureId::ResourcePacksInfoPacketHandle);
    if (addr != 0) {
        m_func1Target = (void*)addr;
        bedrocktoolsplus::hooks::install(m_func1Target, (void*)ResourcePacksInfoPacket_handle_hook, (void**)&original_ResourcePacksInfoPacket_handle);
        m_func1_hooked = true;
    }
}

void ForceGlobalRPModule::initFunc2() {
    if (m_func2Target) return;
    
    uintptr_t addr = bedrocktoolsplus::memory::resolve(bedrocktoolsplus::memory::SignatureId::ResourcePackStackPacketHandle);
    if (addr != 0) {
        m_func2Target = (void*)addr;
        bedrocktoolsplus::hooks::install(m_func2Target, (void*)ResourcePackStackPacket_handle_hook, (void**)&original_ResourcePackStackPacket_handle);
        m_func2_hooked = true;
    }
}

ForceGlobalRPModule::ForceGlobalRPModule()
    : Module("ForceGlobalRP", "force to use global resource packs if the server has the `Required Packs` Option enabled & forcing the use of Vibrant Visuals on a server that forces it to be turned off.") {
    g_forceGlobalRPMod = this;
}

ForceGlobalRPModule::~ForceGlobalRPModule() {
    if (g_forceGlobalRPMod == this) g_forceGlobalRPMod = nullptr;
}

void ForceGlobalRPModule::onInit() {
    initFunc1();
    initFunc2();
}

void ForceGlobalRPModule::onEnable() {
}

void ForceGlobalRPModule::onDisable() {
}

