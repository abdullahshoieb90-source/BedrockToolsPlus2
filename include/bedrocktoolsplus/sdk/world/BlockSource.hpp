#pragma once

#include <bedrocktoolsplus/sdk/Memory.hpp>
#include <bedrocktoolsplus/sdk/Offsets.hpp>

namespace bedrocktoolsplus::sdk {

class BlockSource {
public:
    int dimensionId() {
        return virtualCall<int>(this, offsets::VTable::BlockSource_getDimensionId);
    }
};

}
