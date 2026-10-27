#include "engine/platform/dialogs.hpp"

// Linux and other systems: the desktop's own chooser through zenity or kdialog when one is installed. Nothing is linked: the tool is run
// as a child process, and a machine without either gets "no dialog", which callers handle by keeping their text box.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#define ENGINE_HAS_POPEN 1
#endif

namespace engine::platform {

namespace {

#ifdef ENGINE_HAS_POPEN
std::string shell_quote(const std::string& text) {
    std::string out = "'";
    for (const char c : text) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

bool tool_exists(const char* name) {
    const std::string command = std::string("command -v ") + name + " >/dev/null 2>&1";
    return std::system(command.c_str()) == 0;
}

std::optional<std::string> capture(const std::string& command) {
    std::FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return std::nullopt;
    }
    std::string output;
    char buffer[512];
    while (std::fgets(buffer, sizeof buffer, pipe) != nullptr) {
        output += buffer;
    }
    const int status = pclose(pipe);
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }
    if (status != 0 || output.empty()) {
        return std::nullopt;
    }
    return output;
}

std::optional<std::string> run(bool save, bool folders, const std::string& title, const std::string& initial, const std::string& default_name,
                               const std::vector<FileFilter>& filters) {
    if (tool_exists("zenity")) {
        std::string command = "zenity --file-selection --title=" + shell_quote(title);
        if (folders) {
            command += " --directory";
        }
        if (save) {
            command += " --save --confirm-overwrite";
        }
        if (!initial.empty() || !default_name.empty()) {
            command += " --filename=" + shell_quote(initial + (initial.empty() || initial.back() == '/' ? "" : "/") + default_name);
        }
        for (const FileFilter& filter : filters) {
            command += " --file-filter=" + shell_quote(filter.label + " | " + filter.pattern);
        }
        return capture(command);
    }
    if (tool_exists("kdialog")) {
        const std::string start = shell_quote(initial.empty() ? std::string(".") : initial + "/" + default_name);
        std::string command = std::string("kdialog --title ") + shell_quote(title) +
                              (folders ? " --getexistingdirectory " : save ? " --getsavefilename " : " --getopenfilename ") + start;
        if (!filters.empty() && !folders) {
            command += " " + shell_quote(filters.front().pattern);
        }
        return capture(command);
    }
    return std::nullopt;
}
#else
std::optional<std::string> run(bool, bool, const std::string&, const std::string&, const std::string&, const std::vector<FileFilter>&) {
    return std::nullopt;
}
#endif

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
#ifdef ENGINE_HAS_POPEN
    return tool_exists("zenity") || tool_exists("kdialog");
#else
    return false;
#endif
}

}  // namespace engine::platform
