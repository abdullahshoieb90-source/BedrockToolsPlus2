#pragma once

#include <string_view>

namespace bedrocktoolsplus::sdk { class Player; }

namespace bedrocktoolsplus::core {

class InventoryAccess {
public:
    static InventoryAccess& get();
    void initialize();
    int countItems(const sdk::Player* player, std::string_view identifierPart) const;
    int countStack(const void* stack, std::string_view identifierPart) const;

private:
    using GetOffhandSlotFn = const void* (*)(const void*);
    GetOffhandSlotFn m_getOffhandSlot = nullptr;
};

}
