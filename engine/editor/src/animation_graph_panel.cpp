#include "engine/editor/animation_graph_panel.hpp"

#include "engine/core/file_io.hpp"
#include "engine/core/path.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace engine::editor {
namespace {

using animation::AnimationClip;
using animation::AnimationGraphDocument;
using animation::ClipLibrary;
using animation::CompareOp;
using animation::Condition;
using animation::ConditionKind;
using animation::GraphEditResult;
using animation::JointPose;
using animation::JointTrack;
using animation::ParamDecl;
using animation::ParamType;
using animation::Skeleton;
using animation::StateMachine;
using animation::TransitionDecl;

constexpr float kSidePanelFraction = 0.4f;
constexpr float kMaxPreviewStep = 0.1f;  // seconds; a long frame does not jump the preview past its states

const char* const kOpNames[] = {"<", "<=", ">", ">=", "==", "!="};

// The demo rig: two joints ("root" and a "child" one metre up) and three clips. Only the child moves, so the preview shows a bob.
Skeleton demo_skeleton() {
    Skeleton skeleton;
    skeleton.names = {"root", "child"};
    skeleton.parents = {-1, 0};
    skeleton.bind_pose = {JointPose{}, JointPose{glm::vec3(0.0f, 1.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(1.0f)}};
    skeleton.inverse_bind = {glm::mat4(1.0f), glm::inverse(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f)))};
    return skeleton;
}

ClipLibrary demo_clips() {
    AnimationClip idle;
    idle.name = "idle";
    idle.duration = 1.0f;
    idle.looping = true;
    idle.tracks.resize(2);

    AnimationClip walk = idle;
    walk.name = "walk";
    walk.tracks[1].translation = {{0.0f, glm::vec3(0.0f, 1.0f, 0.0f)}, {0.5f, glm::vec3(0.0f, 1.25f, 0.0f)}, {1.0f, glm::vec3(0.0f, 1.0f, 0.0f)}};

    AnimationClip jump = walk;
    jump.name = "jump";
    jump.duration = 0.5f;
    jump.looping = false;
    jump.tracks[1].translation = {{0.0f, glm::vec3(0.0f, 1.0f, 0.0f)}, {0.25f, glm::vec3(0.0f, 1.5f, 0.0f)}, {0.5f, glm::vec3(0.0f, 1.0f, 0.0f)}};

    return ClipLibrary{{"idle", idle}, {"walk", walk}, {"jump", jump}};
}

// A combo over `items`. Returns true when the user picked another item.
bool combo(const char* label, std::size_t& index, const std::vector<std::string>& items) {
    bool changed = false;
    const char* shown = index < items.size() ? items[index].c_str() : "";
    if (ImGui::BeginCombo(label, shown)) {
        for (std::size_t i = 0; i < items.size(); ++i) {
            const bool selected = i == index;
            if (ImGui::Selectable(items[i].c_str(), selected)) {
                index = i;
                changed = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

std::vector<std::string> parameter_names(const animation::StateGraph& graph, std::optional<ParamType> type) {
    std::vector<std::string> names;
    for (const ParamDecl& parameter : graph.parameters) {
        if (!type || parameter.type == *type) {
            names.push_back(parameter.name);
        }
    }
    return names;
}

std::string text_of(const std::array<char, 64>& buffer) {
    return std::string(buffer.data());
}

}  // namespace

void AnimationGraphPanel::draw(bool& open, animation::AnimatorSet* play_animators) {
    ImGui::SetNextWindowSize(ImVec2(960.0f, 640.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Animation Graph", &open)) {
        ImGui::End();
        return;
    }
    toolbar();
    if (!document_) {
        ImGui::TextDisabled("No graph is open. Use New for the demo rig, or give a path and Open.");
    } else {
        const float footer = ImGui::GetFrameHeightWithSpacing();
        const float left = std::max(260.0f, ImGui::GetContentRegionAvail().x * kSidePanelFraction);
        if (ImGui::BeginChild("animation_states", ImVec2(left, -footer), ImGuiChildFlags_Borders)) {
            states_column();
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("animation_right", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders)) {
            transitions_column();
            preview();
            play_monitor(play_animators);
        }
        ImGui::EndChild();
    }
    ImGui::TextWrapped("%s", status_.c_str());
    ImGui::End();
}

void AnimationGraphPanel::toolbar() {
    ImGui::SetNextItemWidth(320.0f);
    ImGui::InputText("##animation_path", path_.data(), path_.size());
    ImGui::SameLine();
    if (ImGui::Button("New")) {
        auto created = AnimationGraphDocument::create(demo_skeleton(), demo_clips());
        if (created.ok()) {
            document_ = std::move(created).value();
            state_ = 0;
            transition_ = 0;
            set_status("created a graph on the demo rig");
        } else {
            set_status("cannot create: " + created.error());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Open")) {
        open_file();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!document_);
    if (ImGui::Button("Save")) {
        save_file();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!document_ || !document_->can_undo());
    if (ImGui::Button("Undo")) {
        document_->undo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!document_ || !document_->can_redo());
    if (ImGui::Button("Redo")) {
        document_->redo();
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
}

void AnimationGraphPanel::states_column() {
    AnimationGraphDocument& document = *document_;
    const animation::StateGraph& graph = document.graph();
    const std::vector<std::string> clips = document.compatible_clips();

    ImGui::Text("Skeleton: %u joints (demo rig)", document.skeleton().joint_count());
    ImGui::Separator();

    ImGui::TextUnformatted("Parameters");
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputText("name##parameter", new_parameter_name_.data(), new_parameter_name_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    combo("type##parameter", parameter_type_, {"float", "bool", "trigger"});
    ImGui::SameLine();
    if (ImGui::Button("Add##parameter") && new_parameter_name_[0] != '\0') {
        ParamDecl parameter;
        parameter.name = text_of(new_parameter_name_);
        parameter.type = static_cast<ParamType>(parameter_type_);
        if (applied(document.add_parameter(parameter))) {
            new_parameter_name_[0] = '\0';
        }
        return;
    }
    const std::vector<ParamDecl> parameters = graph.parameters;
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const char* type_name = parameters[i].type == ParamType::Float ? "float" : parameters[i].type == ParamType::Bool ? "bool" : "trigger";
        ImGui::Text("%s : %s", parameters[i].name.c_str(), type_name);
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            applied(document.remove_parameter(parameters[i].name));
        }
        ImGui::PopID();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("States");
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputText("name##state", new_state_name_.data(), new_state_name_.size());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    if (clip_choice_ >= clips.size()) {
        clip_choice_ = 0;
    }
    combo("clip##state", clip_choice_, clips);
    ImGui::SameLine();
    ImGui::BeginDisabled(clips.empty());
    if (ImGui::Button("Add##state") && new_state_name_[0] != '\0') {
        if (applied(document.add_state(text_of(new_state_name_), clips[clip_choice_]))) {
            new_state_name_[0] = '\0';
        }
    }
    ImGui::EndDisabled();

    const std::vector<animation::StateDecl> states = graph.states;
    const std::string default_name = graph.default_state;
    for (std::size_t i = 0; i < states.size(); ++i) {
        const std::string label = states[i].name + (states[i].name == default_name ? "  (default)" : "");
        if (ImGui::Selectable(label.c_str(), i == state_)) {
            state_ = i;
        }
    }
    if (state_ >= states.size()) {
        return;
    }

    const animation::StateDecl current = states[state_];
    ImGui::Separator();
    ImGui::Text("State: %s", current.name.c_str());
    std::size_t clip_index = static_cast<std::size_t>(std::find(clips.begin(), clips.end(), current.clip) - clips.begin());
    if (clip_index < clips.size() && combo("clip##selected", clip_index, clips)) {
        applied(document.set_state_clip(current.name, clips[clip_index]));
        return;
    }
    if (clip_index >= clips.size()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "clip \"%s\" is not valid for this skeleton", current.clip.c_str());
    }
    ImGui::SetNextItemWidth(110.0f);
    float speed = current.speed;
    if (ImGui::InputFloat("speed", &speed, 0.0f, 0.0f, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue)) {
        applied(document.set_state_speed(current.name, speed));
        return;
    }
    ImGui::BeginDisabled(current.name == default_name);
    if (ImGui::SmallButton("Make default")) {
        applied(document.set_default_state(current.name));
        return;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove state")) {
        applied(document.remove_state(current.name));
        state_ = 0;
    }
    ImGui::EndDisabled();
}

void AnimationGraphPanel::transitions_column() {
    AnimationGraphDocument& document = *document_;
    const animation::StateGraph& graph = document.graph();
    std::vector<std::string> names;
    for (const animation::StateDecl& state : graph.states) {
        names.push_back(state.name);
    }
    if (from_ >= names.size()) {
        from_ = 0;
    }
    if (to_ >= names.size()) {
        to_ = 0;
    }

    ImGui::TextUnformatted("Transitions");
    ImGui::SetNextItemWidth(130.0f);
    combo("from##transition", from_, names);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    combo("to##transition", to_, names);
    ImGui::SameLine();
    if (ImGui::Button("Add##transition") && !names.empty()) {
        auto added = document.add_transition(names[from_], names[to_]);
        if (added.ok()) {
            transition_ = added.value();
            set_status("");
        } else {
            set_status(added.error());
        }
        return;
    }

    const std::vector<TransitionDecl> transitions = graph.transitions;
    for (std::size_t t = 0; t < transitions.size(); ++t) {
        const std::string label = transitions[t].from + " -> " + transitions[t].to + "  (priority " + std::to_string(transitions[t].priority) + ")";
        if (ImGui::Selectable((label + "##" + std::to_string(t)).c_str(), t == transition_)) {
            transition_ = t;
        }
    }
    if (transition_ >= transitions.size()) {
        return;
    }

    const TransitionDecl current = transitions[transition_];
    ImGui::Separator();
    float duration = current.duration;
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::InputFloat("duration (s)", &duration, 0.0f, 0.0f, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) {
        applied(document.set_transition_timing(transition_, duration, current.priority));
        return;
    }
    int priority = current.priority;
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::InputInt("priority", &priority, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue)) {
        applied(document.set_transition_timing(transition_, current.duration, priority));
        return;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove transition")) {
        applied(document.remove_transition(transition_));
        transition_ = 0;
        return;
    }

    ImGui::TextUnformatted("Conditions (all must hold)");
    for (std::size_t c = 0; c < current.conditions.size(); ++c) {
        condition_row(transition_, c, current.conditions[c]);
    }
    const std::vector<ParamDecl> parameters = graph.parameters;
    ImGui::BeginDisabled(parameters.empty());
    if (ImGui::SmallButton("Add condition")) {
        Condition condition;
        condition.kind = parameters.front().type == ParamType::Float  ? ConditionKind::Float
                         : parameters.front().type == ParamType::Bool ? ConditionKind::Bool
                                                                      : ConditionKind::Trigger;
        condition.parameter = parameters.front().name;
        applied(document.add_condition(transition_, condition));
    }
    ImGui::EndDisabled();
}

void AnimationGraphPanel::condition_row(std::size_t transition, std::size_t index, const Condition& condition) {
    AnimationGraphDocument& document = *document_;
    ImGui::PushID(static_cast<int>(index));
    Condition edited = condition;
    bool changed = false;

    std::size_t kind = static_cast<std::size_t>(condition.kind);
    ImGui::SetNextItemWidth(90.0f);
    if (combo("when", kind, {"float", "bool", "trigger", "finished"})) {
        edited.kind = static_cast<ConditionKind>(kind);
        const std::optional<ParamType> wanted = edited.kind == ConditionKind::Float  ? std::optional(ParamType::Float)
                                                : edited.kind == ConditionKind::Bool ? std::optional(ParamType::Bool)
                                                : edited.kind == ConditionKind::Trigger ? std::optional(ParamType::Trigger)
                                                                                         : std::nullopt;
        const auto choices = wanted ? parameter_names(document.graph(), wanted) : std::vector<std::string>{};
        edited.parameter = choices.empty() ? std::string() : choices.front();
        changed = true;
    }

    if (edited.kind != ConditionKind::Finished) {
        const std::optional<ParamType> wanted = edited.kind == ConditionKind::Float  ? std::optional(ParamType::Float)
                                                : edited.kind == ConditionKind::Bool ? std::optional(ParamType::Bool)
                                                                                     : std::optional(ParamType::Trigger);
        const std::vector<std::string> choices = parameter_names(document.graph(), wanted);
        std::size_t choice = static_cast<std::size_t>(std::find(choices.begin(), choices.end(), edited.parameter) - choices.begin());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130.0f);
        if (!choices.empty() && combo("parameter", choice, choices)) {
            edited.parameter = choices[choice];
            changed = true;
        }
    }
    if (edited.kind == ConditionKind::Float) {
        std::size_t op = static_cast<std::size_t>(edited.op);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        if (combo("op", op, std::vector<std::string>(std::begin(kOpNames), std::end(kOpNames)))) {
            edited.op = static_cast<CompareOp>(op);
            changed = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        // Enter commits a typed value, so the document is not edited on every keystroke.
        if (ImGui::InputFloat("value", &edited.value, 0.0f, 0.0f, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) {
            changed = true;
        }
    } else if (edited.kind == ConditionKind::Bool) {
        ImGui::SameLine();
        if (ImGui::Checkbox("is", &edited.expected)) {
            changed = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) {
        applied(document.remove_condition(transition, index));
    } else if (changed) {
        applied(document.set_condition(transition, index, edited));
    }
    ImGui::PopID();
}

void AnimationGraphPanel::preview() {
    AnimationGraphDocument& document = *document_;
    ImGui::Separator();
    ImGui::TextUnformatted("Preview");
    if (!preview_ || preview_revision_ != document.revision()) {
        auto machine = document.preview();
        if (machine.ok()) {
            preview_ = std::move(machine).value();
        } else {
            preview_.reset();
            set_status("preview: " + machine.error());
        }
        preview_revision_ = document.revision();
    }
    if (!preview_) {
        return;
    }

    ImGui::Checkbox("playing", &playing_);
    ImGui::SameLine();
    if (ImGui::SmallButton("Restart")) {
        preview_.reset();
        return;
    }
    if (playing_) {
        preview_->update(std::min(ImGui::GetIO().DeltaTime, kMaxPreviewStep));
    }

    const std::string state_name(preview_->state_name());
    ImGui::Text("State: %s", state_name.c_str());
    if (preview_->blending()) {
        const animation::StateGraph& graph = document.graph();
        const std::string from = preview_->source_state() < graph.states.size() ? graph.states[preview_->source_state()].name : "?";
        ImGui::Text("Blending from %s", from.c_str());
        ImGui::ProgressBar(preview_->blend_weight(), ImVec2(-1.0f, 0.0f));
    } else {
        const auto clip = document.clips().find(document.graph().states[preview_->state()].clip);
        const float duration = clip == document.clips().end() ? 1.0f : clip->second.duration;
        const float fraction = std::fmod(std::abs(preview_->state_time()), duration) / duration;
        ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), "timeline");
    }

    const std::vector<ParamDecl> parameters = document.graph().parameters;
    for (const ParamDecl& parameter : parameters) {
        ImGui::PushID(parameter.name.c_str());
        if (parameter.type == ParamType::Float) {
            float value = preview_->get_float(parameter.name).value_or(0.0f);
            if (ImGui::SliderFloat(parameter.name.c_str(), &value, -1.0f, 2.0f)) {
                preview_->set_float(parameter.name, value);
            }
        } else if (parameter.type == ParamType::Bool) {
            bool value = preview_->get_bool(parameter.name).value_or(false);
            if (ImGui::Checkbox(parameter.name.c_str(), &value)) {
                preview_->set_bool(parameter.name, value);
            }
        } else if (ImGui::SmallButton(("fire " + parameter.name).c_str())) {
            preview_->set_trigger(parameter.name);
        }
        ImGui::PopID();
    }
}

void AnimationGraphPanel::play_monitor(animation::AnimatorSet* play_animators) {
    ImGui::Separator();
    ImGui::TextUnformatted("Play monitor");
    if (play_animators == nullptr) {
        ImGui::TextDisabled("Start Play to watch the machines that its scripts drive.");
        return;
    }
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputText("machine##monitor", monitor_name_.data(), monitor_name_.size());
    StateMachine* live = play_animators->find(monitor_name_.data());
    if (live == nullptr) {
        ImGui::TextDisabled("No machine of that name runs in the Play session.");
        return;
    }
    const std::string state_name(live->state_name());
    ImGui::Text("State: %s%s", state_name.c_str(), live->blending() ? " (blending)" : "");
    for (const ParamDecl& parameter : live->graph().parameters) {
        if (parameter.type == ParamType::Float) {
            ImGui::Text("%s = %.3f", parameter.name.c_str(), live->get_float(parameter.name).value_or(0.0f));
        } else if (parameter.type == ParamType::Bool) {
            ImGui::Text("%s = %s", parameter.name.c_str(), live->get_bool(parameter.name).value_or(false) ? "true" : "false");
        } else {
            ImGui::Text("%s = %s", parameter.name.c_str(), live->get_trigger(parameter.name).value_or(false) ? "set" : "clear");
        }
    }
}

void AnimationGraphPanel::open_file() {
    const std::string path = path_.data();
    const auto graph_path = core::Path::from_utf8(path);
    if (!graph_path.ok()) {
        set_status("cannot open: the path is not valid UTF-8 or is empty");
        return;
    }
    const auto text = core::read_text_file(graph_path.value());
    if (!text.ok()) {
        set_status("cannot open: " + core::describe(text.error()));
        return;
    }
    auto opened = AnimationGraphDocument::open(text.value(), demo_skeleton(), demo_clips());
    if (!opened.ok()) {
        set_status("cannot open: " + opened.error());
        return;
    }
    document_ = std::move(opened).value();
    state_ = 0;
    transition_ = 0;
    set_status("opened " + path);
}

void AnimationGraphPanel::save_file() {
    const std::string path = path_.data();
    const auto graph_path = core::Path::from_utf8(path);
    if (!graph_path.ok()) {
        set_status("cannot save: the path is not valid UTF-8 or is empty");
        return;
    }
    const auto written = core::write_file_atomic(graph_path.value(), document_->save());
    if (!written.ok()) {
        set_status("cannot save: " + core::describe(written.error()));
        return;
    }
    set_status("saved " + path);
}

bool AnimationGraphPanel::applied(const GraphEditResult& result) {
    if (result.ok()) {
        set_status("");
        return true;
    }
    set_status(result.error());
    return false;
}

void AnimationGraphPanel::set_status(std::string text) {
    status_ = std::move(text);
}

}  // namespace engine::editor
