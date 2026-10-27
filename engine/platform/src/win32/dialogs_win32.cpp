#include "engine/platform/dialogs.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <objbase.h>
#include <shobjidl.h>

#include <string>
#include <vector>

namespace engine::platform {

namespace {

std::wstring to_wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count);
    return out;
}

std::string to_utf8(const wchar_t* text) {
    const int count = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(count - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), count, nullptr, nullptr);
    return out;
}

// COM is initialized for the calling thread once, in the apartment mode the shell dialogs need. A thread that already chose another mode
// keeps it: the dialogs still work, so the failure is ignored.
void ensure_com() {
    thread_local bool done = false;
    if (!done) {
        static_cast<void>(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
        done = true;
    }
}

void set_start_folder(IFileDialog& dialog, const std::string& initial) {
    if (initial.empty()) {
        return;
    }
    IShellItem* item = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(to_wide(initial).c_str(), nullptr, IID_PPV_ARGS(&item))) && item != nullptr) {
        dialog.SetFolder(item);
        item->Release();
    }
}

void set_filters(IFileDialog& dialog, const std::vector<FileFilter>& filters, std::vector<std::wstring>& storage,
                 std::vector<COMDLG_FILTERSPEC>& specs) {
    if (filters.empty()) {
        return;
    }
    storage.reserve(filters.size() * 2);
    for (const FileFilter& filter : filters) {
        storage.push_back(to_wide(filter.label));
        storage.push_back(to_wide(filter.pattern));
    }
    for (std::size_t i = 0; i < filters.size(); ++i) {
        specs.push_back(COMDLG_FILTERSPEC{storage[i * 2].c_str(), storage[i * 2 + 1].c_str()});
    }
    dialog.SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
}

std::optional<std::string> run(bool save, bool folders, const std::string& title, const std::string& initial,
                               const std::string& default_name, const std::vector<FileFilter>& filters) {
    ensure_com();
    IFileDialog* dialog = nullptr;
    const HRESULT created = save ? CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))
                                 : CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(created) || dialog == nullptr) {
        return std::nullopt;
    }
    std::optional<std::string> result;
    DWORD options = 0;
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
    if (folders) {
        options |= FOS_PICKFOLDERS;
    }
    dialog->SetOptions(options);
    dialog->SetTitle(to_wide(title).c_str());
    if (!default_name.empty()) {
        dialog->SetFileName(to_wide(default_name).c_str());
    }
    set_start_folder(*dialog, initial);
    std::vector<std::wstring> storage;
    std::vector<COMDLG_FILTERSPEC> specs;
    if (!folders) {
        set_filters(*dialog, filters, storage, specs);
    }
    if (SUCCEEDED(dialog->Show(GetActiveWindow()))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
                result = to_utf8(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

}  // namespace

std::optional<std::string> pick_folder(const std::string& title, const std::string& initial) {
    return run(false, true, title, initial, {}, {});
}

std::optional<std::string> pick_file(const std::string& title, const std::string& initial, const std::vector<FileFilter>& filters) {
    return run(false, false, title, initial, {}, filters);
}

std::optional<std::string> pick_save_file(const std::string& title, const std::string& initial, const std::string& default_name,
                                          const std::vector<FileFilter>& filters) {
    return run(true, false, title, initial, default_name, filters);
}

bool dialogs_available() {
    return true;
}

}  // namespace engine::platform
