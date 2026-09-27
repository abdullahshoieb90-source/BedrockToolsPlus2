#pragma once

#include <bedrocktoolsplus/sdk/Memory.hpp>
#include <bedrocktoolsplus/sdk/Offsets.hpp>
#include <bedrocktoolsplus/sdk/Types.hpp>

namespace bedrocktoolsplus::sdk {

// Mirrors the game's HitResult layout:
//   Vec3 mStartPos; Vec3 mRayDir; int mType; uchar mFacing; BlockPos mBlockPos; Vec3 mPos;
class HitResult {
public:
    int type() const { return field<int>(this, offsets::HitResult::mType); }
    const Vec3& startPosition() const { return field<Vec3>(this, offsets::HitResult::mStartPos); }
    const Vec3& rayDirection() const { return field<Vec3>(this, offsets::HitResult::mRayDir); }
    const BlockPos& blockPosition() const { return field<BlockPos>(this, offsets::HitResult::mBlockPos); }
    const Vec3& position() const { return field<Vec3>(this, offsets::HitResult::mPos); }
};

}
