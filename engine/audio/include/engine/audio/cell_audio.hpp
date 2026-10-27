#pragma once

// Cell-scoped sound (US-057). Voices that a cell's emitters start carry the cell's owner tag, and the cell's unload releases them, so a
// cell that streams out leaves no sound behind. The subscriber runs on the thread that calls StreamingManager::safe_point(), and it
// touches only the audio engine's mutex-guarded state, never the ECS. The AudioEngine is not owned and must outlive the subscriber:
// the streaming manager's destructor unloads resident cells through their subscribers.

#include "engine/audio/mixer.hpp"
#include "engine/world/partition.hpp"
#include "engine/world/streaming.hpp"

namespace engine::audio {

class AudioEngine;

class CellAudioSubscriber final : public world::CellSubscriber {
public:
    explicit CellAudioSubscriber(AudioEngine& audio) noexcept : audio_(&audio) {}

    // Releases every voice tagged with the unloaded cell's owner. Other voices, including a script's, keep playing.
    void on_unloaded(const world::CellData& cell) override;

    // The owner tag of a cell's voices. Bit 63 is set, so it is never zero and never matches a script owner (an object address). Cell
    // indices stay within +-2^20 (partition.cpp), so each coordinate fits its own 32-bit field and each cell maps to its own tag.
    [[nodiscard]] static OwnerId cell_owner(world::CellCoord coord) noexcept;

private:
    AudioEngine* audio_;
};

}  // namespace engine::audio
