#pragma once

#include <bedrocktoolsplus/sdk/Memory.hpp>
#include <bedrocktoolsplus/sdk/Offsets.hpp>
#include <bedrocktoolsplus/sdk/Types.hpp>

namespace bedrocktoolsplus::sdk {

class HitResult {
public:
    int type() const { return field<int>(this, offsets::HitResult::mType); }
    const Vec3& startPosition() const { return field<Vec3>(this, offsets::HitResult::mStartPos); }
    const Vec3& position() const { return field<Vec3>(this, offsets::HitResult::mPos); }
};

}
