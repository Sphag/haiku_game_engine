#include "engine/editor/material_graph_panel.hpp"

#include "engine/core/file_io.hpp"
#include "engine/core/path.hpp"
#include "engine/material_graph/graph.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace engine::editor {
namespace {

using material_graph::GraphDocument;
using material_graph::Node;
using material_graph::NodeKind;
using material_graph::PinSpec;
using material_graph::ValueType;

constexpr float kNodeWidth = 220.0f;
constexpr float kTitleHeight = 24.0f;
constexpr float kPinRow = 18.0f;
constexpr float kPinRadius = 5.0f;
constexpr float kPinHitRadius = 8.0f;
constexpr float kLinkReach = 60.0f;
constexpr float kSidePanelWidth = 300.0f;
constexpr const char* kLayoutSuffix = ".layout.json";
constexpr float kPi = 3.14159265358979f;
// The sphere samples the surface on this grid, and draws kSphereCells across its diameter.
constexpr std::size_t kPreviewTexels = 64;
constexpr int kSphereCells = 48;

constexpr std::array<NodeKind, 6> kAddableKinds = {NodeKind::Constant,     NodeKind::Parameter, NodeKind::Uv,
                                                   NodeKind::TextureSample, NodeKind::Arithmetic, NodeKind::NormalMap};
constexpr std::array<const char*, 4> kTypeLabels = {"float", "vec2", "vec3", "vec4"};
constexpr std::array<const char*, 6> kOpLabels = {"add", "subtract", "multiply", "divide", "min", "max"};
constexpr std::array<const char*, 4> kComponentLabels = {"x", "y", "z", "w"};

ImU32 colour(int r, int g, int b, int a = 255) {
    return IM_COL32(r, g, b, a);
}

ImVec2 offset(ImVec2 base, ImVec2 by) {
    return ImVec2(base.x + by.x, base.y + by.y);
}

// A node as the canvas draws it this frame. The boxes are copies, so an edit made later in the frame cannot leave a dangling pointer.
struct Box {
    std::string id;
    NodeKind kind = NodeKind::Constant;
    ImVec2 min{0.0f, 0.0f};
    ImVec2 max{0.0f, 0.0f};
    std::map<std::string, std::string> inputs;
    std::vector<PinSpec> pins;
};

bool has_output(NodeKind kind) {
    return kind != NodeKind::PbrOutput;
}

float node_height(const std::vector<PinSpec>& pins) {
    const std::size_t rows = std::max<std::size_t>(pins.size(), 1);
    return kTitleHeight + static_cast<float>(rows) * kPinRow + 6.0f;
}

ImVec2 input_pin(const Box& box, std::size_t pin) {
    return ImVec2(box.min.x, box.min.y + kTitleHeight + (static_cast<float>(pin) + 0.5f) * kPinRow);
}

ImVec2 output_pin(const Box& box) {
    return ImVec2(box.max.x, box.min.y + kTitleHeight * 0.5f);
}

bool near(ImVec2 a, ImVec2 b) {
    return std::fabs(a.x - b.x) <= kPinHitRadius && std::fabs(a.y - b.y) <= kPinHitRadius;
}

bool inside(const Box& box, ImVec2 point) {
    return point.x >= box.min.x && point.x <= box.max.x && point.y >= box.min.y && point.y <= box.max.y;
}

std::optional<std::size_t> hit_output(const std::vector<Box>& boxes, ImVec2 mouse) {
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        if (has_output(boxes[i].kind) && near(output_pin(boxes[i]), mouse)) {
            return i;
        }
    }
    return std::nullopt;
}

struct Pick {
    std::size_t box = 0;
    std::size_t pin = 0;
};

std::optional<Pick> hit_input(const std::vector<Box>& boxes, ImVec2 mouse) {
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        for (std::size_t pin = 0; pin < boxes[i].pins.size(); ++pin) {
            if (near(input_pin(boxes[i], pin), mouse)) {
                return Pick{i, pin};
            }
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> hit_node(const std::vector<Box>& boxes, ImVec2 mouse) {
    for (std::size_t i = boxes.size(); i-- > 0;) {
        if (inside(boxes[i], mouse)) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> find_box(const std::vector<Box>& boxes, const std::string& id) {
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        if (boxes[i].id == id) {
            return i;
        }
    }
    return std::nullopt;
}

void draw_link(ImDrawList* draw, ImVec2 from, ImVec2 to, ImU32 color) {
    draw->AddBezierCubic(from, ImVec2(from.x + kLinkReach, from.y), ImVec2(to.x - kLinkReach, to.y), to, color, 2.0f);
}

std::string error_summary(const std::vector<material_graph::GraphError>& errors) {
    std::string text;
    for (const material_graph::GraphError& error : errors) {
        text += material_graph::to_text(error) + "; ";
    }
    return text;
}

}  // namespace

void MaterialGraphPanel::set_status(std::string text) {
    status_ = std::move(text);
}

void MaterialGraphPanel::draw(bool& open) {
    ImGui::SetNextWindowSize(ImVec2(960.0f, 620.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Material Graph", &open)) {
        ImGui::End();
        return;
    }
    toolbar();
    if (!document_) {
        ImGui::TextDisabled("No material is open. Use New, or give a path and Open.");
    } else {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float canvas_width = std::max(200.0f, avail.x - kSidePanelWidth - ImGui::GetStyle().ItemSpacing.x);
        canvas(canvas_width, avail.y);
        ImGui::SameLine();
        side_column(kSidePanelWidth, avail.y);

        // Shortcuts act when no text field has the keyboard and this window has focus.
        const ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !document_->selection().empty()) {
                const std::vector<std::string> ids(document_->selection().begin(), document_->selection().end());
                const auto removed = document_->remove_nodes(ids);
                if (!removed.ok()) {
                    set_status(removed.error());
                }
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
                static_cast<void>(io.KeyShift ? document_->redo() : document_->undo());
            } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
                static_cast<void>(document_->redo());
            }
        }
    }
    ImGui::End();
}

void MaterialGraphPanel::toolbar() {
    ImGui::SetNextItemWidth(300.0f);
    ImGui::InputText("##material_path", path_.data(), path_.size());
    ImGui::SameLine();
    if (ImGui::Button("New")) {
        document_ = GraphDocument::create("untitled");
        set_status("new material");
    }
    ImGui::SameLine();
    if (ImGui::Button("Open")) {
        open_file();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!document_.has_value());
    if (ImGui::Button("Save")) {
        save_file();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!document_.has_value() || !document_->can_undo());
    if (ImGui::Button("Undo")) {
        static_cast<void>(document_->undo());
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!document_.has_value() || !document_->can_redo());
    if (ImGui::Button("Redo")) {
        static_cast<void>(document_->redo());
    }
    ImGui::EndDisabled();
    ImGui::SameLine();

    ImGui::SetNextItemWidth(120.0f);
    const std::string add_label(material_graph::to_string(kAddableKinds[add_kind_]));
    if (ImGui::BeginCombo("##add_kind", add_label.c_str())) {
        for (std::size_t i = 0; i < kAddableKinds.size(); ++i) {
            const std::string label(material_graph::to_string(kAddableKinds[i]));
            if (ImGui::Selectable(label.c_str(), i == add_kind_)) {
                add_kind_ = i;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!document_.has_value());
    if (ImGui::Button("Add node")) {
        // The node lands at the centre of the visible canvas, which the pan offset moves.
        const auto added = document_->add_node(kAddableKinds[add_kind_], glm::vec2(-pan_.x + 40.0f, -pan_.y + 40.0f));
        if (!added.ok()) {
            set_status(added.error());
        }
    }
    ImGui::EndDisabled();
    ImGui::TextUnformatted(status_.c_str());
}

void MaterialGraphPanel::canvas(float width, float height) {
    GraphDocument& doc = *document_;
    ImGui::BeginChild("##graph_canvas", ImVec2(width, height), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsWindowHovered();
    const ImVec2 origin = offset(ImGui::GetCursorScreenPos(), pan_);
    const ImVec2 mouse = io.MousePos;

    // The boxes, in the order the graph lists the nodes. A node that is being dragged is drawn where the drag has put it.
    std::vector<Box> boxes;
    for (const Node& node : doc.graph().nodes) {
        Box box;
        box.id = node.id;
        box.kind = node.kind;
        box.inputs = node.inputs;
        box.pins = material_graph::pin_specs(node.kind);
        ImVec2 at(0.0f, 0.0f);
        const auto placed = doc.layout().find(node.id);
        if (placed != doc.layout().end()) {
            at = ImVec2(placed->second.x, placed->second.y);
        }
        if (moving_ && doc.selection().contains(node.id)) {
            at = offset(at, move_offset_);
        }
        box.min = offset(origin, at);
        box.max = ImVec2(box.min.x + kNodeWidth, box.min.y + node_height(box.pins));
        boxes.push_back(std::move(box));
    }

    // Links first, so the nodes are drawn over their ends.
    for (const Box& consumer : boxes) {
        for (std::size_t pin = 0; pin < consumer.pins.size(); ++pin) {
            const auto linked = consumer.inputs.find(consumer.pins[pin].name);
            if (linked == consumer.inputs.end()) {
                continue;
            }
            const auto source = find_box(boxes, linked->second);
            if (source) {
                draw_link(draw, output_pin(boxes[*source]), input_pin(consumer, pin), colour(170, 190, 220));
            }
        }
    }
    if (linking_) {
        const auto source = find_box(boxes, link_source_);
        if (source) {
            draw_link(draw, output_pin(boxes[*source]), mouse, colour(250, 220, 90));
        }
    }

    for (const Box& box : boxes) {
        const bool selected = doc.selection().contains(box.id);
        const bool failed = doc.node_has_error(box.id);
        draw->AddRectFilled(box.min, box.max, colour(38, 42, 50), 6.0f);
        draw->AddRectFilled(box.min, ImVec2(box.max.x, box.min.y + kTitleHeight), colour(60, 90, 140), 6.0f, ImDrawFlags_RoundCornersTop);
        const std::string title = box.id + " : " + std::string(material_graph::to_string(box.kind));
        draw->AddText(ImVec2(box.min.x + 8.0f, box.min.y + 4.0f), failed ? colour(255, 150, 150) : IM_COL32_WHITE, title.c_str());
        const ImU32 border = failed ? colour(230, 70, 70) : (selected ? colour(250, 220, 90) : colour(90, 95, 105));
        draw->AddRect(box.min, box.max, border, 6.0f, 0, (failed || selected) ? 2.5f : 1.0f);
        for (std::size_t pin = 0; pin < box.pins.size(); ++pin) {
            const ImVec2 at = input_pin(box, pin);
            const bool connected = box.inputs.contains(box.pins[pin].name);
            draw->AddCircleFilled(at, kPinRadius, connected ? colour(120, 200, 140) : colour(150, 150, 160));
            draw->AddText(ImVec2(at.x + 9.0f, at.y - 7.0f), colour(210, 210, 220), box.pins[pin].name.c_str());
        }
        if (has_output(box.kind)) {
            draw->AddCircleFilled(output_pin(box), kPinRadius, colour(120, 170, 230));
        }
    }

    // Mouse: press to select, drag to move, drag from an output to an input to link, right-click an input to unlink, middle-drag to pan.
    if (hovered && ImGui::IsMouseClicked(0)) {
        if (const auto output = hit_output(boxes, mouse)) {
            linking_ = true;
            link_source_ = boxes[*output].id;
        } else if (const auto node = hit_node(boxes, mouse)) {
            const std::string id = boxes[*node].id;
            if (io.KeyCtrl) {
                doc.select(id, true);
            } else if (!doc.selection().contains(id)) {
                doc.select(id, false);
            }
            moving_ = doc.selection().contains(id);
            move_offset_ = ImVec2(0.0f, 0.0f);
        } else if (!io.KeyCtrl) {
            doc.clear_selection();
        }
    }
    if (hovered && ImGui::IsMouseClicked(1)) {
        if (const auto input = hit_input(boxes, mouse)) {
            const std::string consumer = boxes[input->box].id;
            const std::string pin = boxes[input->box].pins[input->pin].name;
            const auto unlinked = doc.unlink(consumer, pin);
            if (!unlinked.ok()) {
                set_status(unlinked.error());
            }
        }
    }
    if (hovered && ImGui::IsMouseDown(2)) {
        pan_ = offset(pan_, io.MouseDelta);
    }
    if (moving_) {
        if (ImGui::IsMouseDown(0)) {
            move_offset_ = offset(move_offset_, io.MouseDelta);
        }
        if (ImGui::IsMouseReleased(0)) {
            moving_ = false;
            if (move_offset_.x != 0.0f || move_offset_.y != 0.0f) {
                const std::vector<std::string> ids(doc.selection().begin(), doc.selection().end());
                const auto moved = doc.move_nodes(ids, glm::vec2(move_offset_.x, move_offset_.y));
                if (!moved.ok()) {
                    set_status(moved.error());
                }
            }
            move_offset_ = ImVec2(0.0f, 0.0f);
        }
    }
    if (linking_ && ImGui::IsMouseReleased(0)) {
        linking_ = false;
        if (const auto input = hit_input(boxes, mouse)) {
            const std::string consumer = boxes[input->box].id;
            const std::string pin = boxes[input->box].pins[input->pin].name;
            const auto linked = doc.link(consumer, pin, link_source_);
            if (!linked.ok()) {
                set_status(linked.error());
            }
        }
    }
    ImGui::EndChild();
}

void MaterialGraphPanel::side_column(float width, float height) {
    ImGui::BeginChild("##graph_side", ImVec2(width, height), ImGuiChildFlags_Borders);
    properties();
    ImGui::Separator();
    sphere_preview(std::min(width - 24.0f, 240.0f));
    ImGui::Separator();
    error_list();
    ImGui::EndChild();
}

void MaterialGraphPanel::properties() {
    GraphDocument& doc = *document_;

    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputText("##texture_name", texture_name_.data(), texture_name_.size());
    ImGui::SameLine();
    if (ImGui::Button("Declare texture") && texture_name_[0] != '\0') {
        material_graph::TextureDecl decl;
        decl.name = texture_name_.data();
        const auto declared = doc.add_texture(decl);
        if (!declared.ok()) {
            set_status(declared.error());
        }
    }
    ImGui::Separator();

    if (doc.selection().size() != 1) {
        if (doc.selection().empty()) {
            ImGui::TextDisabled("Select a node to edit it.");
        } else {
            ImGui::TextDisabled("%zu nodes selected.", doc.selection().size());
        }
        return;
    }

    // A copy: the edits below change the document, which replaces its node storage.
    const std::string id = *doc.selection().begin();
    const auto found = std::find_if(doc.graph().nodes.begin(), doc.graph().nodes.end(), [wanted = id](const Node& node) { return node.id == wanted; });
    if (found == doc.graph().nodes.end()) {
        return;
    }
    const Node node = *found;
    ImGui::Text("%s", id.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", std::string(material_graph::to_string(node.kind)).c_str());

    if (node.kind == NodeKind::Constant || node.kind == NodeKind::Parameter) {
        ImGui::SetNextItemWidth(140.0f);
        int type = static_cast<int>(node.type);
        if (ImGui::Combo("type", &type, kTypeLabels.data(), static_cast<int>(kTypeLabels.size()))) {
            const auto changed = doc.set_type(id, static_cast<ValueType>(type));
            if (!changed.ok()) {
                set_status(changed.error());
            }
        }
        // Enter commits a component, so a value typed in part is not an edit.
        const int components = static_cast<int>(material_graph::components(node.type));
        for (int i = 0; i < components; ++i) {
            float value = node.value[i];
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::InputFloat(kComponentLabels[static_cast<std::size_t>(i)], &value, 0.0f, 0.0f, "%.3f",
                                  ImGuiInputTextFlags_EnterReturnsTrue)) {
                glm::vec4 next = node.value;
                next[i] = value;
                const auto changed = doc.set_value(id, next);
                if (!changed.ok()) {
                    set_status(changed.error());
                }
            }
            ImGui::PopID();
        }
    }
    if (node.kind == NodeKind::Parameter) {
        std::array<char, 128> name{};
        std::snprintf(name.data(), name.size(), "%s", node.name.c_str());
        if (ImGui::InputText("parameter", name.data(), name.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
            const auto renamed = doc.set_name(id, name.data());
            if (!renamed.ok()) {
                set_status(renamed.error());
            }
        }
    }
    if (node.kind == NodeKind::TextureSample) {
        // The choice is made from the list and applied after it, because applying it replaces the list's storage.
        std::string chosen;
        if (ImGui::BeginCombo("texture", node.name.c_str())) {
            for (const material_graph::TextureDecl& texture : doc.graph().textures) {
                if (ImGui::Selectable(texture.name.c_str(), texture.name == node.name)) {
                    chosen = texture.name;
                }
            }
            ImGui::EndCombo();
        }
        if (!chosen.empty()) {
            const auto picked = doc.set_name(id, chosen);
            if (!picked.ok()) {
                set_status(picked.error());
            }
        }
    }
    if (node.kind == NodeKind::Arithmetic) {
        ImGui::SetNextItemWidth(140.0f);
        int op = static_cast<int>(node.op);
        if (ImGui::Combo("operation", &op, kOpLabels.data(), static_cast<int>(kOpLabels.size()))) {
            const auto changed = doc.set_op(id, static_cast<material_graph::ArithmeticOp>(op));
            if (!changed.ok()) {
                set_status(changed.error());
            }
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Inputs");
    for (const PinSpec& pin : material_graph::pin_specs(node.kind)) {
        ImGui::TextUnformatted(pin.name.c_str());
        ImGui::SameLine();
        const auto linked = node.inputs.find(pin.name);
        if (linked == node.inputs.end()) {
            ImGui::TextDisabled("not linked");
            continue;
        }
        ImGui::Text("<- %s", linked->second.c_str());
        ImGui::SameLine();
        ImGui::PushID(pin.name.c_str());
        if (ImGui::SmallButton("Unlink")) {
            const auto unlinked = doc.unlink(id, pin.name);
            if (!unlinked.ok()) {
                set_status(unlinked.error());
            }
        }
        ImGui::PopID();
    }

    if (node.kind != NodeKind::PbrOutput) {
        ImGui::Separator();
        if (ImGui::Button("Delete node")) {
            const auto removed = doc.remove_nodes({id});
            if (!removed.ok()) {
                set_status(removed.error());
            }
        }
    }
}

void MaterialGraphPanel::sphere_preview(float size) {
    GraphDocument& doc = *document_;
    if (!preview_sampled_ || preview_revision_ != doc.revision()) {
        preview_sampled_ = true;
        preview_revision_ = doc.revision();
        auto sampled = doc.preview(kPreviewTexels);
        if (sampled.ok()) {
            swatch_ = std::move(sampled).value();
        } else {
            swatch_ = material_graph::PreviewSwatch{};
        }
        preview_message_ = doc.valid() ? std::string() : std::string("The graph has errors. The preview shows its last valid version.");
        if (swatch_.size == 0) {
            preview_message_ = "There is no valid version to preview yet.";
        }
    }

    ImGui::TextUnformatted("Preview");
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at, ImVec2(at.x + size, at.y + size), colour(24, 26, 32));
    if (swatch_.size > 0) {
        // The sphere is lit by one fixed light. Its colour is the base colour read from the swatch at the sphere's UV, so the preview
        // shows the parameter values the evaluator holds. Normal maps and roughness are not shaded here.
        const float cell = size / static_cast<float>(kSphereCells);
        const glm::vec3 light = glm::normalize(glm::vec3(0.4f, 0.6f, 0.7f));
        const auto texels = static_cast<int>(swatch_.size);
        for (int row = 0; row < kSphereCells; ++row) {
            for (int column = 0; column < kSphereCells; ++column) {
                const float x = (static_cast<float>(column) + 0.5f) / static_cast<float>(kSphereCells) * 2.0f - 1.0f;
                const float y = 1.0f - (static_cast<float>(row) + 0.5f) / static_cast<float>(kSphereCells) * 2.0f;
                const float radius_squared = x * x + y * y;
                if (radius_squared > 1.0f) {
                    continue;
                }
                const glm::vec3 normal(x, y, std::sqrt(1.0f - radius_squared));
                const float u = std::atan2(x, normal.z) / (2.0f * kPi) + 0.5f;
                const float v = std::asin(y) / kPi + 0.5f;
                const int texel_column = std::clamp(static_cast<int>(u * static_cast<float>(texels)), 0, texels - 1);
                const int texel_row = std::clamp(static_cast<int>(v * static_cast<float>(texels)), 0, texels - 1);
                const glm::vec4 base = swatch_.base_color[static_cast<std::size_t>(texel_row * texels + texel_column)];
                const float diffuse = 0.25f + 0.75f * std::max(glm::dot(normal, light), 0.0f);
                const glm::vec3 lit = glm::clamp(glm::vec3(base) * diffuse, 0.0f, 1.0f);
                // Base colours are linear; the screen shows sRGB.
                const glm::vec3 screen = glm::pow(lit, glm::vec3(1.0f / 2.2f));
                const ImVec2 corner(at.x + static_cast<float>(column) * cell, at.y + static_cast<float>(row) * cell);
                draw->AddRectFilled(corner, ImVec2(corner.x + cell, corner.y + cell),
                                    colour(static_cast<int>(screen.x * 255.0f), static_cast<int>(screen.y * 255.0f),
                                           static_cast<int>(screen.z * 255.0f)));
            }
        }
    }
    if (!preview_message_.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + size);
        ImGui::TextDisabled("%s", preview_message_.c_str());
        ImGui::PopTextWrapPos();
    }
}

void MaterialGraphPanel::error_list() {
    GraphDocument& doc = *document_;
    ImGui::Text("Errors: %zu", doc.errors().size());
    if (doc.valid()) {
        ImGui::TextDisabled("The graph is valid.");
        return;
    }
    // Choosing an error selects the node it names, so the properties show it.
    for (const material_graph::GraphError& error : doc.errors()) {
        const std::string text = material_graph::to_text(error);
        if (ImGui::Selectable(text.c_str()) && !error.node.empty()) {
            doc.clear_selection();
            doc.select(error.node, false);
        }
    }
}

void MaterialGraphPanel::open_file() {
    const std::string path = path_.data();
    const auto graph_path = core::Path::from_utf8(path);
    const auto layout_path = core::Path::from_utf8(path + kLayoutSuffix);
    if (!graph_path.ok() || !layout_path.ok()) {
        set_status("cannot open: the path is not valid UTF-8 or is empty");
        return;
    }
    const auto text = core::read_text_file(graph_path.value());
    if (!text.ok()) {
        set_status("cannot open: " + core::describe(text.error()));
        return;
    }
    // The layout is optional: a graph with no layout file places its nodes on the grid.
    std::string layout;
    const auto layout_text = core::read_text_file(layout_path.value());
    if (layout_text.ok()) {
        layout = layout_text.value();
    }
    auto opened = GraphDocument::open(text.value(), layout);
    if (!opened.ok()) {
        set_status("cannot open: " + error_summary(opened.error()));
        return;
    }
    document_ = std::move(opened).value();
    set_status("opened " + path);
}

void MaterialGraphPanel::save_file() {
    const std::string path = path_.data();
    const auto graph_path = core::Path::from_utf8(path);
    const auto layout_path = core::Path::from_utf8(path + kLayoutSuffix);
    if (!graph_path.ok() || !layout_path.ok()) {
        set_status("not saved: the path is not valid UTF-8 or is empty");
        return;
    }
    const auto graph = document_->save_graph();
    if (!graph.ok()) {
        set_status("not saved, the graph has errors: " + error_summary(graph.error()));
        return;
    }
    const auto written = core::write_file_atomic(graph_path.value(), graph.value());
    if (!written.ok()) {
        set_status("not saved: " + core::describe(written.error()));
        return;
    }
    const auto layout = core::write_file_atomic(layout_path.value(), document_->save_layout());
    if (!layout.ok()) {
        set_status("graph saved, layout not saved: " + core::describe(layout.error()));
        return;
    }
    set_status("saved " + path);
}

}  // namespace engine::editor
