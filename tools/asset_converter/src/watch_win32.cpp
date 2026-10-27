// Source watching (US-025): the Windows backend, on ReadDirectoryChangesW. One overlapped read covers the whole tree, so new
// directories need no extra watch.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "asset_converter/watch.hpp"

#include "engine/core/path.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace engine::converter {

namespace {

namespace fs = std::filesystem;

constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                                FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;

std::string utf8_text(const fs::path& value) {
    const auto text = value.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

class DirectoryChangeWatch final : public ChangeSource {
public:
    DirectoryChangeWatch(HANDLE directory, HANDLE event) : directory_(directory), event_(event), buffer_(kBufferWords) {}

    ~DirectoryChangeWatch() override {
        // The buffer and the OVERLAPPED record belong to a read the OS may still fill, so the read ends before they go.
        if (pending_) {
            CancelIoEx(directory_, &overlapped_);
            DWORD ignored = 0;
            GetOverlappedResult(directory_, &overlapped_, &ignored, TRUE);
        }
        CloseHandle(event_);
        CloseHandle(directory_);
    }

    // Starts the first read. A failed start is reported to the caller, because nothing would be watched.
    bool start() {
        return issue_read();
    }

    WatchEvents poll(std::chrono::milliseconds timeout) override {
        WatchEvents result;
        if (!pending_ && !issue_read()) {
            // The read could not be restarted. Report a rescan and wait, so a broken watch does not spin the caller.
            result.overflow = true;
            Sleep(static_cast<DWORD>(timeout.count()));
            return result;
        }
        if (WaitForSingleObject(event_, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) {
            return result;
        }

        pending_ = false;
        DWORD bytes = 0;
        if (!GetOverlappedResult(directory_, &overlapped_, &bytes, FALSE)) {
            result.overflow = true;
        } else if (bytes == 0) {
            // The notification buffer overflowed, so some changes were lost and the next pass rescans everything.
            result.overflow = true;
        } else {
            parse(result);
        }
        issue_read();
        return result;
    }

private:
    static constexpr std::size_t kBufferWords = 16 * 1024;  // 64 KiB of notifications per read

    bool issue_read() {
        ResetEvent(event_);
        overlapped_ = {};
        overlapped_.hEvent = event_;
        DWORD ignored = 0;
        const BOOL ok = ReadDirectoryChangesW(directory_, buffer_.data(), static_cast<DWORD>(buffer_.size() * sizeof(DWORD)),
                                              TRUE, kNotifyFilter, &ignored, &overlapped_, nullptr);
        pending_ = ok != 0 || GetLastError() == ERROR_IO_PENDING;
        return pending_;
    }

    // Reads the notifications in the buffer. Names are relative to the watched root, with '/' after conversion.
    void parse(WatchEvents& result) {
        const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer_.data());
        for (;;) {
            const std::wstring name(info->FileName, info->FileNameLength / sizeof(WCHAR));
            result.paths.push_back(utf8_text(fs::path(name)));
            if (info->NextEntryOffset == 0) {
                break;
            }
            info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(reinterpret_cast<const std::uint8_t*>(info) +
                                                                    info->NextEntryOffset);
        }
    }

    HANDLE directory_;
    HANDLE event_;
    OVERLAPPED overlapped_{};
    bool pending_ = false;
    std::vector<DWORD> buffer_;
};

}  // namespace

core::Result<std::unique_ptr<ChangeSource>, std::string> open_directory_watch(const core::Path& root) {
    const fs::path native = core::to_native_path(root);
    HANDLE directory = CreateFileW(native.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        return core::failure("cannot open " + root.str() + " for watching: Windows error " + std::to_string(GetLastError()));
    }
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) {
        const DWORD error = GetLastError();
        CloseHandle(directory);
        return core::failure("cannot create a watch event: Windows error " + std::to_string(error));
    }
    auto watch = std::make_unique<DirectoryChangeWatch>(directory, event);
    if (!watch->start()) {
        return core::failure("cannot watch " + root.str() + ": Windows error " + std::to_string(GetLastError()));
    }
    return core::success(std::unique_ptr<ChangeSource>(std::move(watch)));
}

}  // namespace engine::converter
