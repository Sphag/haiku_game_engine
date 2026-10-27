#pragma once

// The cooked container (US-019 writes it, US-024 reads it): every "<guid>.cooked" file is this layout around one importer's
// payload. Little-endian throughout:
//   magic "OWCK" (4 bytes), u32 container version, u64 payload size, then exactly that many payload bytes.
// The container holds no timestamps or paths, so its bytes depend only on the payload. The runtime reads it without the
// converter, which is why the reader lives here.

#include "engine/core/path.hpp"
#include "engine/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace engine::assets {

inline constexpr std::string_view kCookedContainerMagic = "OWCK";
inline constexpr std::uint32_t kCookedContainerVersion = 1;
inline constexpr std::size_t kCookedContainerHeaderBytes = 16;  // magic, version, and payload size

// Wraps a payload in a container.
[[nodiscard]] std::string write_cooked_container(std::string_view payload);

// Checks the magic, the version, and that the payload size matches the bytes that follow the header. The returned view
// points into `bytes`, so the caller must keep those bytes alive while it uses the payload.
[[nodiscard]] core::Result<std::string_view, std::string> read_cooked_container(std::string_view bytes);

// Reads a whole cooked file. The bytes still need read_cooked_container().
[[nodiscard]] core::Result<std::string, std::string> read_cooked_file(const core::Path& path);

}  // namespace engine::assets
