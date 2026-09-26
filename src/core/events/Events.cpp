#include <bedrocktoolsplus/events/EventBus.hpp>

namespace bedrocktoolsplus::events {

EventBus& bus() {
    static EventBus instance;
    return instance;
}

}
