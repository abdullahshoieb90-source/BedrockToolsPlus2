#pragma once

#include <bedrocktoolsplus/events/Event.hpp>

namespace bedrocktoolsplus::sdk { class Player; }

namespace bedrocktoolsplus::events {

struct LocalPlayerPreTickEvent {
    static constexpr EventType type = EventType::LocalPlayerPreTick;
    sdk::Player* player;
};

}
