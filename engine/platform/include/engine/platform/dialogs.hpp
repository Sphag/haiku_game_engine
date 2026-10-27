#pragma once

// The operating system's own file and folder dialogs, so a tool never asks a person to type a path. Each call blocks until the dialog
// closes and returns the chosen path in UTF-8, or nothing when the person cancelled or no dialog is available on this machine (Linux
// without zenity or kdialog). A caller treats "nothing" as "keep the text box".

#include <optional>
#include <string>
#include <vector>

namespace engine::platform {

struct FileFilter {
    std::string label;    // "Scenes"
    std::string pattern;  // "*.scene" (one pattern; several are separated by ';' on Windows and spaces on Linux)
};

// Chooses an existing folder. `initial` is where the dialog starts; an empty or missing folder starts where the system chooses.
[[nodiscard]] std::optional<std::string> pick_folder(const std::string& title, const std::string& initial);

// Chooses an existing file.
[[nodiscard]] std::optional<std::string> pick_file(const std::string& title, const std::string& initial, const std::vector<FileFilter>& filters);

// Chooses a file name to write; it may not exist yet.
[[nodiscard]] std::optional<std::string> pick_save_file(const std::string& title, const std::string& initial, const std::string& default_name,
                                                        const std::vector<FileFilter>& filters);

// True when this machine can show the dialogs above.
[[nodiscard]] bool dialogs_available();

}  // namespace engine::platform
