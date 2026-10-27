#include "engine/editor/asset_browser.hpp"

#include "engine/core/file_io.hpp"
#include "engine/core/guid.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <utility>

namespace engine::editor {

namespace {

constexpr std::string_view kCategory = "assets";
constexpr std::uint32_t kThumbnailSide = 96;

// Lower-case copy, for case-insensitive search. Only ASCII letters change, so UTF-8 bytes pass through unchanged.
std::string lowered(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string_view importer_or_none(const CatalogEntry& entry) {
    return entry.asset ? std::string_view(entry.asset->importer) : std::string_view("none");
}

}  // namespace

AssetBrowser::AssetBrowser(std::shared_ptr<Console> console) : console_(std::move(console)) {}

AssetBrowser::~AssetBrowser() {
    close();
}

void AssetBrowser::open(const core::Path& project_root, const core::Path& assets_root, const assets::ProjectScan& scan) {
    close();
    project_root_ = project_root;
    assets_root_ = assets_root;
    open_ = true;
    load_catalog(scan);
    open_watch();
}

void AssetBrowser::close() {
    // The worker reads the importers and writes the cancel flag and counter, so it is stopped and waited for before the state goes.
    if (future_.valid()) {
        cancel_flag_.store(true, std::memory_order_release);
        future_.wait();
        // The result is discarded here; an exception from the run must not leave the destructor.
        try {
            static_cast<void>(future_.get());
        } catch (...) {
        }
    }
    watch_.reset();
    debouncer_.clear();
    watch_failed_logged_ = false;
    open_ = false;
    project_root_.reset();
    assets_root_.reset();
    entries_.clear();
    scan_.reset();
    selected_.clear();
    previews_.clear();
    import_diagnostics_.clear();
    import_summary_.reset();
    import_total_ = 0;
    cancel_flag_.store(false, std::memory_order_release);
    completed_.store(0, std::memory_order_release);
}

void AssetBrowser::refresh(const assets::ProjectScan& scan) {
    if (!open_) {
        return;
    }
    load_catalog(scan);
}

bool AssetBrowser::poll_changes() {
    if (!watch_) {
        return false;
    }
    const auto now = converter::Debouncer::Clock::now();
    const converter::WatchEvents events = watch_->poll(std::chrono::milliseconds(0));
    if (!events.paths.empty() || events.overflow) {
        debouncer_.note(now);
    }
    if (!debouncer_.due(now)) {
        return false;
    }
    debouncer_.clear();
    return true;
}

void AssetBrowser::load_catalog(const assets::ProjectScan& scan) {
    // The previous outcomes are kept by GUID, so a rescan does not forget the last import.
    std::map<std::string, converter::AssetOutcome> outcomes;
    for (const CatalogEntry& entry : entries_) {
        if (entry.asset && entry.last) {
            outcomes.emplace(entry.asset->guid.to_string(), *entry.last);
        }
    }

    entries_.clear();
    for (const assets::AssetRecord* record : scan.registry.records()) {
        CatalogEntry entry;
        entry.source = record->source_path.str();
        entry.asset = *record;
        if (const auto found = outcomes.find(record->guid.to_string()); found != outcomes.end()) {
            entry.last = found->second;
        }
        entries_.push_back(std::move(entry));
    }
    for (const std::string& source : scan.unregistered) {
        CatalogEntry entry;
        entry.source = source;
        entries_.push_back(std::move(entry));
    }
    std::sort(entries_.begin(), entries_.end(), [](const CatalogEntry& lhs, const CatalogEntry& rhs) { return lhs.source < rhs.source; });

    scan_ = scan;
    // A selection whose GUID left the catalog is cleared; any other stays, so a rescan keeps the user's place.
    if (!selected_.empty() && !index_of(selected_)) {
        selected_.clear();
    }
}

std::vector<const CatalogEntry*> AssetBrowser::search(std::string_view query) const {
    const std::string needle = lowered(query);
    std::vector<const CatalogEntry*> matches;
    for (const CatalogEntry& entry : entries_) {
        if (needle.empty()) {
            matches.push_back(&entry);
            continue;
        }
        const std::string guid = entry.asset ? entry.asset->guid.to_string() : std::string();
        if (lowered(entry.source).find(needle) != std::string::npos || lowered(importer_or_none(entry)).find(needle) != std::string::npos ||
            guid.find(needle) != std::string::npos) {
            matches.push_back(&entry);
        }
    }
    return matches;
}

const CatalogEntry* AssetBrowser::find(std::string_view guid) const {
    const auto index = index_of(guid);
    return index ? &entries_[*index] : nullptr;
}

void AssetBrowser::select(std::string_view guid) {
    if (guid.empty()) {
        selected_.clear();
        return;
    }
    const auto index = index_of(guid);
    selected_ = index && entries_[*index].asset ? entries_[*index].asset->guid.to_string() : std::string();
}

const CatalogEntry* AssetBrowser::selected() const {
    return selected_.empty() ? nullptr : find(selected_);
}

const Preview& AssetBrowser::preview(const CatalogEntry& entry) {
    const std::string key = entry.asset ? entry.asset->guid.to_string() : "source:" + entry.source;
    const std::string hash = entry.asset ? entry.asset->content_hash.to_hex() : std::string();
    if (const auto found = previews_.find(key); found != previews_.end() && found->second.content_hash == hash) {
        return found->second;
    }

    Preview preview;
    preview.content_hash = hash;
    if (!entry.asset) {
        preview.error = "the source has no metadata, so it has no GUID and no preview";
    } else if (entry.asset->importer != "texture") {
        preview.error = "no preview for the " + entry.asset->importer + " importer";
    } else if (!assets_root_) {
        preview.error = "no project is open";
    } else if (auto file = assets_root_->join(entry.source); !file.ok()) {
        preview.error = "the source path is not valid";
    } else if (auto bytes = core::read_file(file.value()); !bytes.ok()) {
        preview.error = "the source cannot be read: " + core::describe(bytes.error());
    } else if (auto decoded = converter::make_texture_preview(bytes.value(), kThumbnailSide); !decoded.ok()) {
        preview.error = decoded.error();
    } else {
        Thumbnail thumbnail;
        thumbnail.width = decoded.value().width;
        thumbnail.height = decoded.value().height;
        thumbnail.pixels = std::move(decoded.value().pixels);
        preview.image = std::move(thumbnail);
    }
    previews_[key] = std::move(preview);
    return previews_[key];
}

std::optional<std::size_t> AssetBrowser::index_of(std::string_view guid) const {
    auto parsed = core::Guid::parse(std::string(guid));
    if (!parsed.ok()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].asset && entries_[i].asset->guid == parsed.value()) {
            return i;
        }
    }
    return std::nullopt;
}

core::Result<core::Unit, std::string> AssetBrowser::check_registered(std::string_view guid, const CatalogEntry*& entry) const {
    if (!open_) {
        return core::failure(std::string("no project is open"));
    }
    entry = find(guid);
    if (entry == nullptr) {
        return core::failure("no asset has GUID " + std::string(guid));
    }
    if (!entry->registered()) {
        return core::failure("the source " + entry->source + " has no metadata, so it has no GUID; add its sidecar first");
    }
    return core::success();
}

core::Result<core::Unit, std::string> AssetBrowser::set_settings(std::string_view guid, std::string_view settings_json) {
    const CatalogEntry* entry = nullptr;
    if (auto ok = check_registered(guid, entry); !ok.ok()) {
        return ok;
    }
    const converter::Importer* importer = importers_.find(entry->asset->importer);
    if (importer == nullptr) {
        return core::failure("the importer " + entry->asset->importer + " is not registered");
    }
    auto canonical = importer->canonical_settings(settings_json);
    if (!canonical.ok()) {
        note(core::LogLevel::Error, entry->source + ": the import settings were not saved: " + canonical.error());
        return core::failure("the settings are invalid: " + canonical.error());
    }
    auto source = core::Path::from_utf8(entry->source);
    if (!source.ok()) {
        return core::failure(std::string("the source path is not valid UTF-8"));
    }
    if (auto written = assets::update_asset_metadata(*assets_root_, source.value(), canonical.value()); !written.ok()) {
        note(core::LogLevel::Error, entry->source + ": cannot write the sidecar: " + written.error().message);
        return core::failure("cannot write the sidecar: " + written.error().message);
    }
    note(core::LogLevel::Info, entry->source + ": import settings saved");
    return core::success();
}

core::Result<std::vector<std::string>, std::string> AssetBrowser::references_to(std::string_view guid) const {
    const CatalogEntry* entry = nullptr;
    if (auto ok = check_registered(guid, entry); !ok.ok()) {
        return core::failure(ok.error());
    }
    const core::Guid target = entry->asset->guid;
    const std::string needle = target.to_string();
    std::vector<std::string> lines;
    for (const core::Guid& dependent : scan_->registry.dependents(target)) {
        const assets::AssetRecord* record = scan_->registry.find(dependent);
        lines.push_back("asset " + (record ? record->source_path.str() : dependent.to_string()) + " depends on it");
    }
    for (const assets::AssetRecord* record : scan_->registry.records()) {
        if (record->guid == target || (record->importer != "json" && record->importer != "text")) {
            continue;
        }
        auto file = assets_root_->join(record->source_path.str());
        if (!file.ok()) {
            continue;
        }
        auto text = core::read_text_file(file.value());
        if (!text.ok()) {
            continue;
        }
        if (lowered(text.value()).find(needle) != std::string::npos) {
            lines.push_back(record->source_path.str() + " names its GUID");
        }
    }
    return core::success(std::move(lines));
}

core::Result<core::Unit, std::string> AssetBrowser::rename(std::string_view guid, std::string_view new_source) {
    const CatalogEntry* entry = nullptr;
    if (auto ok = check_registered(guid, entry); !ok.ok()) {
        return ok;
    }
    const std::string old_source = entry->source;
    if (new_source.empty() || new_source == old_source) {
        return core::failure(std::string("choose a new path for ") + old_source);
    }
    auto from = core::Path::from_utf8(old_source);
    auto to = core::Path::from_utf8(std::string(new_source));
    if (!from.ok() || !to.ok()) {
        return core::failure(std::string("the path is not valid UTF-8"));
    }
    if (auto moved = assets::move_asset(*assets_root_, from.value(), to.value()); !moved.ok()) {
        note(core::LogLevel::Error, "cannot rename " + old_source + ": " + moved.error().message);
        return core::failure(moved.error().message);
    }
    note(core::LogLevel::Info, "renamed " + old_source + " to " + std::string(new_source));
    return core::success();
}

core::Result<core::Unit, std::string> AssetBrowser::remove(std::string_view guid) {
    const CatalogEntry* entry = nullptr;
    if (auto ok = check_registered(guid, entry); !ok.ok()) {
        return ok;
    }
    const std::string source = entry->source;
    auto path = core::Path::from_utf8(source);
    if (!path.ok()) {
        return core::failure(std::string("the source path is not valid UTF-8"));
    }
    if (auto removed = assets::remove_asset(*assets_root_, path.value()); !removed.ok()) {
        note(core::LogLevel::Error, "cannot delete " + source + ": " + removed.error().message);
        return core::failure(removed.error().message);
    }
    note(core::LogLevel::Info, "deleted " + source);
    return core::success();
}

core::Result<core::Unit, std::string> AssetBrowser::start_import() {
    if (!open_) {
        return core::failure(std::string("no project is open"));
    }
    if (importing()) {
        return core::failure(std::string("an import is already running"));
    }
    cancel_flag_.store(false, std::memory_order_release);
    completed_.store(0, std::memory_order_release);
    import_total_ = scan_ ? scan_->registry.size() : 0;
    import_diagnostics_.clear();
    import_summary_.reset();

    auto output = project_root_->join("cooked");
    if (!output.ok()) {
        return core::failure(std::string("the output path is not valid UTF-8"));
    }
    // The fields are in declaration order: project, output, jobs (0 for the hardware count), cancel, completed.
    const converter::Options options{*assets_root_, output.value(), 0, &cancel_flag_, &completed_};
    future_ = std::async(std::launch::async, [this, options]() { return converter::run(converter::Command::Cook, options, importers_); });
    note(core::LogLevel::Info, "import started: " + std::to_string(import_total_) + " registered asset(s)");
    return core::success();
}

void AssetBrowser::cancel_import() noexcept {
    cancel_flag_.store(true, std::memory_order_release);
}

std::size_t AssetBrowser::import_done() const noexcept {
    return completed_.load(std::memory_order_acquire);
}

bool AssetBrowser::poll_import() {
    if (!future_.valid() || future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return false;
    }
    core::Result<converter::Report, std::string> result = core::failure(std::string("the import stopped unexpectedly"));
    try {
        result = future_.get();
    } catch (const std::exception& error) {
        result = core::failure(std::string(error.what()));
    }
    if (!result.ok()) {
        import_summary_ = "import failed: " + result.error();
        note(core::LogLevel::Error, *import_summary_);
        return true;
    }
    const converter::Report& report = result.value();

    std::size_t converted = 0;
    std::size_t up_to_date = 0;
    std::size_t failed = 0;
    std::size_t blocked = 0;
    for (const converter::AssetOutcome& outcome : report.assets) {
        if (const auto index = index_of(outcome.guid)) {
            entries_[*index].last = outcome;
        }
        switch (outcome.status) {
            case converter::AssetStatus::Imported:
            case converter::AssetStatus::Cooked:
                ++converted;
                break;
            case converter::AssetStatus::UpToDate:
                ++up_to_date;
                break;
            case converter::AssetStatus::Failed:
                ++failed;
                break;
            case converter::AssetStatus::Blocked:
                ++blocked;
                break;
            case converter::AssetStatus::Valid:
                break;
        }
    }
    import_diagnostics_ = report.diagnostics;
    for (const converter::Diagnostic& diagnostic : report.diagnostics) {
        const core::LogLevel level = diagnostic.severity == converter::Severity::Error ? core::LogLevel::Error : core::LogLevel::Warning;
        std::string line = "[" + std::string(converter::to_string(diagnostic.code)) + "] ";
        if (!diagnostic.source.empty()) {
            line += diagnostic.source + ": ";
        }
        note(level, line + diagnostic.message);
    }

    const std::size_t untouched = report.assets.size() - converted - up_to_date - failed - blocked;
    std::string summary = "import finished: " + std::to_string(converted) + " converted, " + std::to_string(up_to_date) +
                          " up to date, " + std::to_string(failed) + " failed, " + std::to_string(blocked) + " blocked";
    if (report.cancelled) {
        summary += "; cancelled with " + std::to_string(untouched) + " not converted";
    }
    import_summary_ = summary;
    note(failed > 0 || blocked > 0 ? core::LogLevel::Warning : core::LogLevel::Info, summary);
    return true;
}

void AssetBrowser::open_watch() {
    watch_.reset();
    debouncer_.clear();
    watch_failed_logged_ = false;
    if (!assets_root_) {
        return;
    }
    auto watch = converter::open_directory_watch(*assets_root_);
    if (!watch.ok()) {
        note(core::LogLevel::Warning, "the assets folder is not watched, so changes need a restart: " + watch.error());
        watch_failed_logged_ = true;
        return;
    }
    watch_ = std::move(watch).value();
}

void AssetBrowser::note(core::LogLevel level, std::string_view message) {
    console_->add(level, kCategory, message);
}

}  // namespace engine::editor
