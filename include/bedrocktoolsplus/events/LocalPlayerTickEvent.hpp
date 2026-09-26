#pragma once

#include <bedrocktoolsplus/events/Event.hpp>

namespace bedrocktoolsplus::sdk { class Player; }

namespace bedrocktoolsplus::events {

struct LocalPlayerTickEvent {
    static constexpr EventType type = EventType::LocalPlayerTick;
    sdk::Player* player;
};

}
