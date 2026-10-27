#include "asset_converter/importer.hpp"

#include "engine/core/utf8.hpp"

#include <nlohmann/json.hpp>

#include <utility>

namespace engine::converter {
namespace {

using Json = nlohmann::json;
using core::Result;
using core::Unit;
using core::failure;
using core::success;

// Settings are a JSON object. Anything else, including text that is not JSON, is rejected with a message.
Result<Json, std::string> parse_settings_object(std::string_view text) {
    Json settings = Json::parse(std::string(text), nullptr, false);
    if (settings.is_discarded() || !settings.is_object()) {
        return failure(std::string("settings must be a JSON object"));
    }
    return success(std::move(settings));
}

// An importer without settings accepts only the empty object, and its canonical form is "{}".
Result<std::string, std::string> no_settings(std::string_view settings_json) {
    auto parsed = parse_settings_object(settings_json);
    if (!parsed.ok()) {
        return failure(parsed.error());
    }
    if (!parsed.value().empty()) {
        return failure(std::string("this importer takes no settings"));
    }
    return success(std::string("{}"));
}

class BlobImporter final : public Importer {
public:
    [[nodiscard]] std::string_view type() const noexcept override {
        return "blob";
    }
    [[nodiscard]] int version() const noexcept override {
        return 1;
    }
    [[nodiscard]] Result<std::string, std::string> canonical_settings(std::string_view settings_json) const override {
        return no_settings(settings_json);
    }
    [[nodiscard]] Result<ImportOutput, std::string> import(std::string_view source, std::string_view) const override {
        return success(ImportOutput{std::string(source), {}});
    }
};

class TextImporter final : public Importer {
public:
    [[nodiscard]] std::string_view type() const noexcept override {
        return "text";
    }
    [[nodiscard]] int version() const noexcept override {
        return 1;
    }

    [[nodiscard]] Result<std::string, std::string> canonical_settings(std::string_view settings_json) const override {
        auto parsed = parse_settings_object(settings_json);
        if (!parsed.ok()) {
            return failure(parsed.error());
        }
        const Json& settings = parsed.value();
        bool normalize = false;
        for (auto it = settings.begin(); it != settings.end(); ++it) {
            if (it.key() != "normalize_newlines") {
                return failure("unknown setting '" + it.key() + "'");
            }
            if (!it->is_boolean()) {
                return failure(std::string("setting 'normalize_newlines' must be a boolean"));
            }
            normalize = it->get<bool>();
        }
        Json canonical = Json::object();
        canonical["normalize_newlines"] = normalize;
        return success(canonical.dump());
    }

    [[nodiscard]] Result<ImportOutput, std::string> import(std::string_view source,
                                                           std::string_view settings) const override {
        if (!core::is_valid_utf8(source)) {
            return failure(std::string("source is not valid UTF-8"));
        }
        auto parsed = parse_settings_object(settings);
        if (!parsed.ok()) {
            return failure(parsed.error());
        }
        const bool normalize = parsed.value().value("normalize_newlines", false);
        if (!normalize) {
            return success(ImportOutput{std::string(source), {}});
        }
        std::string text;
        text.reserve(source.size());
        for (std::size_t i = 0; i < source.size(); ++i) {
            // "\r\n" becomes "\n". A lone "\r" is kept, because it is not a line break in this format.
            if (source[i] == '\r' && i + 1 < source.size() && source[i + 1] == '\n') {
                continue;
            }
            text.push_back(source[i]);
        }
        return success(ImportOutput{std::move(text), {}});
    }
};

class JsonImporter final : public Importer {
public:
    [[nodiscard]] std::string_view type() const noexcept override {
        return "json";
    }
    [[nodiscard]] int version() const noexcept override {
        return 1;
    }
    [[nodiscard]] Result<std::string, std::string> canonical_settings(std::string_view settings_json) const override {
        return no_settings(settings_json);
    }
    [[nodiscard]] Result<ImportOutput, std::string> import(std::string_view source, std::string_view) const override {
        Json document = Json::parse(std::string(source), nullptr, false);
        if (document.is_discarded()) {
            return failure(std::string("source is not valid JSON"));
        }
        // nlohmann keeps object keys sorted and emits no whitespace, so equal documents give equal bytes.
        return success(ImportOutput{document.dump(), {}});
    }
};

}  // namespace

std::unique_ptr<Importer> make_blob_importer() {
    return std::make_unique<BlobImporter>();
}

std::unique_ptr<Importer> make_text_importer() {
    return std::make_unique<TextImporter>();
}

std::unique_ptr<Importer> make_json_importer() {
    return std::make_unique<JsonImporter>();
}

Result<Unit, std::string> ImporterRegistry::add(std::unique_ptr<Importer> importer) {
    if (!importer) {
        return failure(std::string("importer is null"));
    }
    const std::string_view type = importer->type();
    if (type.empty()) {
        return failure(std::string("importer type is empty"));
    }
    if (find(type) != nullptr) {
        return failure("importer '" + std::string(type) + "' is already registered");
    }
    importers_.push_back(std::move(importer));
    return success();
}

const Importer* ImporterRegistry::find(std::string_view type) const {
    for (const auto& importer : importers_) {
        if (importer->type() == type) {
            return importer.get();
        }
    }
    return nullptr;
}

ImporterRegistry ImporterRegistry::builtin() {
    ImporterRegistry registry;
    registry.importers_.push_back(make_blob_importer());
    registry.importers_.push_back(make_text_importer());
    registry.importers_.push_back(make_json_importer());
    registry.importers_.push_back(make_model_importer());
    registry.importers_.push_back(make_texture_importer());
    registry.importers_.push_back(make_gi_probes_importer());
    return registry;
}

}  // namespace engine::converter
