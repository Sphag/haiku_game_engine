#pragma once

// Importers (US-019): each one turns a source's bytes and its canonical settings into an imported payload. The payload is
// what the cook step packs, so two runs with the same source, settings, and importer version produce the same bytes.
//
// The built-in importers are deliberately small: "blob" passes bytes through, "text" checks UTF-8 and can normalize line
// endings, and "json" parses and re-emits canonical JSON. "model" (US-020) converts glTF/GLB, FBX, and OBJ through Assimp.
// "texture" (US-021) decodes PNG/JPEG and cooks every mip. "gi_probes" (US-040) bakes an irradiance probe volume from a JSON scene
// description. Audio importers register through the same interface.

#include "engine/core/result.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace engine::converter {

// What an import produces: the payload, and one note for each part of the source that the importer skipped. Notes are
// warnings; the converter reports them and keeps the payload.
struct ImportOutput {
    std::string payload;
    std::vector<std::string> unsupported;
};

class Importer {
public:
    Importer() = default;
    Importer(const Importer&) = delete;
    Importer& operator=(const Importer&) = delete;
    virtual ~Importer() = default;

    [[nodiscard]] virtual std::string_view type() const noexcept = 0;

    // Bump when the output changes for the same input. The version is part of every cache key of this importer.
    [[nodiscard]] virtual int version() const noexcept = 0;

    // Checks the settings text (a JSON object) and returns its canonical form, with defaults filled in and keys sorted.
    // Settings that differ only in spelling or defaults have the same canonical form, so they share cache entries.
    [[nodiscard]] virtual core::Result<std::string, std::string> canonical_settings(std::string_view settings_json) const = 0;

    // Produces the imported payload from the source and canonical settings. A failure carries a message and leaves nothing behind.
    [[nodiscard]] virtual core::Result<ImportOutput, std::string> import(std::string_view source,
                                                                          std::string_view settings) const = 0;
};

// Owns the importers by type name. Type names are unique and not empty.
class ImporterRegistry {
public:
    ImporterRegistry() = default;

    // Fails with a message when the type name is empty or already registered; the registry is unchanged on failure.
    [[nodiscard]] core::Result<core::Unit, std::string> add(std::unique_ptr<Importer> importer);

    [[nodiscard]] const Importer* find(std::string_view type) const;

    // blob, text, json, model, texture, and gi_probes, at their current versions.
    [[nodiscard]] static ImporterRegistry builtin();

private:
    std::vector<std::unique_ptr<Importer>> importers_;
};

[[nodiscard]] std::unique_ptr<Importer> make_blob_importer();
[[nodiscard]] std::unique_ptr<Importer> make_text_importer();
[[nodiscard]] std::unique_ptr<Importer> make_json_importer();
[[nodiscard]] std::unique_ptr<Importer> make_model_importer();
[[nodiscard]] std::unique_ptr<Importer> make_texture_importer();
[[nodiscard]] std::unique_ptr<Importer> make_gi_probes_importer();

}  // namespace engine::converter
