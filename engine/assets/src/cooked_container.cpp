#include "engine/assets/cooked_container.hpp"

#include "engine/core/file_io.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace engine::assets {

namespace {

void append_little_endian(std::string& out, std::uint64_t value, int byte_count) {
    for (int i = 0; i < byte_count; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFFu));
    }
}

std::uint64_t read_little_endian(std::string_view bytes, std::size_t offset, int byte_count) {
    std::uint64_t value = 0;
    for (int i = 0; i < byte_count; ++i) {
        const auto byte = static_cast<std::uint8_t>(bytes[offset + static_cast<std::size_t>(i)]);
        value |= static_cast<std::uint64_t>(byte) << (8 * i);
    }
    return value;
}

}  // namespace

std::string write_cooked_container(std::string_view payload) {
    std::string out(kCookedContainerMagic);
    append_little_endian(out, kCookedContainerVersion, 4);
    append_little_endian(out, payload.size(), 8);
    out.append(payload);
    return out;
}

core::Result<std::string_view, std::string> read_cooked_container(std::string_view bytes) {
    if (bytes.size() < kCookedContainerHeaderBytes) {
        return core::failure(std::string("cooked file is truncated: ") + std::to_string(bytes.size()) +
                             " bytes, header needs " + std::to_string(kCookedContainerHeaderBytes));
    }
    if (bytes.substr(0, kCookedContainerMagic.size()) != kCookedContainerMagic) {
        return core::failure(std::string("not a cooked container: magic is not \"OWCK\""));
    }
    const auto version = read_little_endian(bytes, 4, 4);
    if (version != kCookedContainerVersion) {
        return core::failure("unsupported cooked container version " + std::to_string(version));
    }
    const auto payload_size = read_little_endian(bytes, 8, 8);
    const auto available = bytes.size() - kCookedContainerHeaderBytes;
    if (payload_size != available) {
        return core::failure("cooked payload size " + std::to_string(payload_size) + " does not match the " +
                             std::to_string(available) + " bytes in the file");
    }
    return core::success(bytes.substr(kCookedContainerHeaderBytes));
}

core::Result<std::string, std::string> read_cooked_file(const core::Path& path) {
    auto bytes = core::read_file(path);
    if (!bytes.ok()) {
        return core::failure("cannot read " + path.str() + ": " + core::describe(bytes.error()));
    }
    return core::success(std::move(bytes).value());
}

}  // namespace engine::assets
