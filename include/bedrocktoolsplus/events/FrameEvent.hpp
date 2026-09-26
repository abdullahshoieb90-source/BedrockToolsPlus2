#pragma once

#include <bedrocktoolsplus/events/Event.hpp>

namespace bedrocktoolsplus::events {

struct FrameEvent {
    static constexpr EventType type = EventType::Frame;
};

}
