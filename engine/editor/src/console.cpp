#include "engine/editor/console.hpp"

#include "engine/core/utf8.hpp"

#include <utility>

namespace engine::editor {

std::string sanitize_utf8(std::string_view text) {
    if (core::is_valid_utf8(text)) {
        return std::string(text);
    }
    // Keep every well-formed sequence and replace each byte that does not start one. A sequence is well formed when it is valid
    // on its own, so a bad byte cannot hide a good character next to it.
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) {
            out.push_back(text[i]);
            ++i;
            continue;
        }
        std::size_t length = 0;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
        }
        if (length != 0 && i + length <= text.size() && core::is_valid_utf8(text.substr(i, length))) {
            out.append(text.substr(i, length));
            i += length;
        } else {
            out.push_back('?');
            ++i;
        }
    }
    return out;
}

Console::Console(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

void Console::write(const core::LogRecord& record) {
    std::lock_guard lock(mutex_);
    push_locked(record.level, record.category, record.message);
}

void Console::add(core::LogLevel level, std::string_view category, std::string_view message) {
    std::lock_guard lock(mutex_);
    push_locked(level, category, message);
}

void Console::push_locked(core::LogLevel level, std::string_view category, std::string_view message) {
    if (entries_.size() == capacity_) {
        entries_.pop_front();
        ++dropped_;
    }
    entries_.push_back(ConsoleEntry{next_sequence_++, level, sanitize_utf8(category), sanitize_utf8(message)});
    if (mirror_ != nullptr) {
        const ConsoleEntry& entry = entries_.back();
        core::LogRecord record{level, entry.category, entry.message, {}};
        mirror_->write(record);
    }
}

void Console::set_mirror(std::shared_ptr<core::LogSink> sink) {
    std::lock_guard lock(mutex_);
    mirror_ = std::move(sink);
}

std::vector<ConsoleEntry> Console::snapshot() const {
    std::lock_guard lock(mutex_);
    return std::vector<ConsoleEntry>(entries_.begin(), entries_.end());
}

void Console::clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
    dropped_ = 0;
}

std::uint64_t Console::dropped() const {
    std::lock_guard lock(mutex_);
    return dropped_;
}

}  // namespace engine::editor
