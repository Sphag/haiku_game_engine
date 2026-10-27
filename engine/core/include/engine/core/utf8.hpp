#pragma once

#include <string_view>

namespace engine::core {

// Strict UTF-8 check (RFC 3629): rejects overlong forms, surrogates, code points above U+10FFFF, and truncation.
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

}  // namespace engine::core
