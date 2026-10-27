#pragma once

// The asset browser (US-066): the project's asset catalog, search, thumbnails, import settings, import with progress and cancel,
// and the rename and delete commands with the references they affect. It has no ImGui or graphics dependency, so the commands are
// tested without a window (docs/editor.md). The Assets panel draws it.
//
// The catalog is the project scan the ProjectSession holds; refresh() takes a copy of it. References are GUIDs, so renaming an asset
// changes only its path, and deleting one leaves the GUIDs that name it in place: they are reported before the delete, so the user
// sees what will go missing.
//
// Hot reload. A directory watch on the assets folder reports changes. poll_changes() turns them into one rescan, debounced like the
// converter's source watch. The rescan keeps the selection (keyed by GUID) and reloads a thumbnail only when its source's bytes changed.
//
// Import runs the converter's cook on a worker thread into <project>/cooked. The run reports its progress as the number of finished
// assets, and cancel() stops it before the next asset starts. Assets that were already published stay on disk, and the rest keep their
// previous output. Import errors are logged to the console and kept with each asset's outcome.

#include "engine/assets/asset_project.hpp"
#include "engine/assets/content_hash.hpp"
#include "engine/core/path.hpp"
#include "engine/core/result.hpp"
#include "engine/editor/console.hpp"

#include "asset_converter/converter.hpp"
#include "asset_converter/importer.hpp"
#include "asset_converter/watch.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

// One source of the project: a registered asset (with its sidecar) or a source that has no sidecar yet.
struct CatalogEntry {
    std::string source;                           // project-relative path, with '/' separators
    std::optional<assets::AssetRecord> asset;     // nullopt when the source has no sidecar and so no GUID
    std::optional<converter::AssetOutcome> last;  // the outcome of the last import run for this asset

    [[nodiscard]] bool registered() const noexcept {
        return asset.has_value();
    }
};

// A thumbnail: RGBA8, row by row from the top.
struct Thumbnail {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
};

// The preview of a texture source. The hash is the source's content hash when the preview was made; a changed source makes a new one.
struct Preview {
    std::string content_hash;
    std::optional<Thumbnail> image;
    std::string error;  // why there is no image; empty when there is one
};

class AssetBrowser {
public:
    // The importers are the converter's built-in set. The browser keeps the console for its import and command messages.
    explicit AssetBrowser(std::shared_ptr<Console> console);
    // Cancels a running import and waits for it, so the worker never outlives the browser.
    ~AssetBrowser();
    AssetBrowser(const AssetBrowser&) = delete;
    AssetBrowser& operator=(const AssetBrowser&) = delete;
    AssetBrowser(AssetBrowser&&) = delete;
    AssetBrowser& operator=(AssetBrowser&&) = delete;

    // Opens the browser on a project: project_root holds project.json and the cooked folder, and assets_root is its assets folder.
    // The catalog is `scan`. A running import is cancelled first.
    void open(const core::Path& project_root, const core::Path& assets_root, const assets::ProjectScan& scan);
    // Forgets the project. A running import is cancelled and waited for.
    void close();
    [[nodiscard]] bool is_open() const noexcept {
        return open_;
    }

    // Takes the scan again after the project changed. The selection stays when its GUID is still registered.
    void refresh(const assets::ProjectScan& scan);
    // Starts watching the assets folder again after close or open. A watch that cannot start is logged and the browser keeps working.
    // Returns true once a debounced change is due, and the caller then rescans the project and calls refresh().
    [[nodiscard]] bool poll_changes();

    [[nodiscard]] const std::vector<CatalogEntry>& entries() const noexcept {
        return entries_;
    }
    // The entries whose source path, importer, or GUID contains the query, ignoring case, in catalog order. An empty query matches all.
    [[nodiscard]] std::vector<const CatalogEntry*> search(std::string_view query) const;
    [[nodiscard]] const CatalogEntry* find(std::string_view guid) const;

    // The selected asset, by GUID text. An empty selection is "none". A selection that leaves the catalog is cleared.
    void select(std::string_view guid);
    [[nodiscard]] const CatalogEntry* selected() const;

    // The preview of a registered asset. Only texture sources have an image; others return a preview with the reason in error.
    [[nodiscard]] const Preview& preview(const CatalogEntry& entry);

    // Replaces the importer settings of a registered asset. The settings are checked by the importer first, so a rejected value
    // leaves the sidecar as it was. The catalog is not refreshed here; the caller rescans the project.
    [[nodiscard]] core::Result<core::Unit, std::string> set_settings(std::string_view guid, std::string_view settings_json);
    // The lines an edit of this asset would affect: assets that depend on it, and text sources (json or text importers) whose text
    // names its GUID. Binary sources are not searched.
    [[nodiscard]] core::Result<std::vector<std::string>, std::string> references_to(std::string_view guid) const;
    // Moves a registered asset's source and sidecar to a new project-relative path. The GUID does not change.
    [[nodiscard]] core::Result<core::Unit, std::string> rename(std::string_view guid, std::string_view new_source);
    // Deletes a registered asset's source and sidecar. Its cooked output is left in the cooked folder.
    [[nodiscard]] core::Result<core::Unit, std::string> remove(std::string_view guid);

    // Import (cook) on a worker thread. Fails when there is no project or an import is already running.
    [[nodiscard]] core::Result<core::Unit, std::string> start_import();
    // Asks the running import to stop before its next asset. Does nothing when no import runs.
    void cancel_import() noexcept;
    [[nodiscard]] bool importing() const noexcept {
        return future_.valid();
    }
    [[nodiscard]] std::size_t import_done() const noexcept;
    [[nodiscard]] std::size_t import_total() const noexcept {
        return import_total_;
    }
    // Collects a finished import: stores its outcomes and diagnostics and logs them. Returns true when an import finished on this call.
    [[nodiscard]] bool poll_import();
    // The diagnostics of the last finished import, warnings and errors in report order.
    [[nodiscard]] const std::vector<converter::Diagnostic>& import_diagnostics() const noexcept {
        return import_diagnostics_;
    }
    [[nodiscard]] const std::optional<std::string>& import_summary() const noexcept {
        return import_summary_;
    }

private:
    void load_catalog(const assets::ProjectScan& scan);
    void note(core::LogLevel level, std::string_view message);
    [[nodiscard]] std::optional<std::size_t> index_of(std::string_view guid) const;
    [[nodiscard]] core::Result<core::Unit, std::string> check_registered(std::string_view guid, const CatalogEntry*& entry) const;
    void open_watch();

    std::shared_ptr<Console> console_;
    converter::ImporterRegistry importers_ = converter::ImporterRegistry::builtin();
    bool open_ = false;
    std::optional<core::Path> project_root_;
    std::optional<core::Path> assets_root_;
    std::vector<CatalogEntry> entries_;
    std::optional<assets::ProjectScan> scan_;
    std::string selected_;
    std::map<std::string, Preview> previews_;  // keyed by GUID text

    std::unique_ptr<converter::ChangeSource> watch_;
    converter::Debouncer debouncer_{converter::WatchOptions{}};
    bool watch_failed_logged_ = false;

    std::future<core::Result<converter::Report, std::string>> future_;
    std::atomic<bool> cancel_flag_{false};
    std::atomic<std::size_t> completed_{0};
    std::size_t import_total_ = 0;
    std::vector<converter::Diagnostic> import_diagnostics_;
    std::optional<std::string> import_summary_;
};

}  // namespace engine::editor
