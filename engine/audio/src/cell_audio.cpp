#include "engine/audio/cell_audio.hpp"

#include "engine/audio/audio_engine.hpp"

#include <cstdint>

namespace engine::audio {

namespace {

// Cell indices are within +-2^20, so this bias maps every one of them to a non-negative 32-bit value.
constexpr std::int64_t kCellIndexBias = std::int64_t{1} << 20;
constexpr std::uint64_t kCellOwnerFlag = std::uint64_t{1} << 63;

}  // namespace

void CellAudioSubscriber::on_unloaded(const world::CellData& cell) {
    if (!cell) {
        return;
    }
    audio_->stop_owner(cell_owner(cell->coord));
}

OwnerId CellAudioSubscriber::cell_owner(world::CellCoord coord) noexcept {
    const auto x = static_cast<std::uint64_t>(static_cast<std::int64_t>(coord.x) + kCellIndexBias);
    const auto z = static_cast<std::uint64_t>(static_cast<std::int64_t>(coord.z) + kCellIndexBias);
    return kCellOwnerFlag | (x << 32) | (z & 0xFFFFFFFFu);
}

}  // namespace engine::audio
