#pragma once

#include <bedrocktoolsplus/sdk/Memory.hpp>
#include <bedrocktoolsplus/sdk/Offsets.hpp>
#include <bedrocktoolsplus/sdk/world/BlockSource.hpp>
#include <bedrocktoolsplus/sdk/world/Weather.hpp>

namespace bedrocktoolsplus::sdk {

class Dimension {
public:
    BlockSource* blockSource() { return field<BlockSource*>(this, offsets::Dimension::mBlockSource); }
    Weather* weather() { return field<Weather*>(this, offsets::Dimension::mWeather); }
};

}
