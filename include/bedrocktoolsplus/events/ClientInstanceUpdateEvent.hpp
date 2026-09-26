#pragma once

#include <bedrocktoolsplus/events/Event.hpp>

namespace bedrocktoolsplus::sdk { class ClientInstance; }

namespace bedrocktoolsplus::events {

struct ClientInstanceUpdateEvent {
    static constexpr EventType type = EventType::ClientInstanceUpdate;
    sdk::ClientInstance* clientInstance;
};

}
