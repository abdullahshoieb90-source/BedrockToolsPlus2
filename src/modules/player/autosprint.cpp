#include "autosprint.hpp"
#include <bedrocktoolsplus/events/EventBus.hpp>
#include <bedrocktoolsplus/sdk/input/MoveInput.hpp>

AutoSprintModule::AutoSprintModule()
    : Module("AutoSprint", "Holds Minecraft's native sprint input for you.") {}

void AutoSprintModule::onInit() {
    bedrocktoolsplus::events::bus().subscribe<bedrocktoolsplus::events::LocalPlayerPreTickEvent>([this](auto& event) {
        if (!enabled || !event.player) return;
        auto* input = bedrocktoolsplus::sdk::moveInputComponent(event.player);
        if (!input) return;
        input->mRawInputState.set(MoveInputState::Flag::SprintDown, true);
    }, bedrocktoolsplus::events::EventPriority::First);
}
