// The game editor (US-061): one native window that hosts the editor shell. It creates the graphics device for the chosen backend, the
// swapchain, and the Dear ImGui context, then runs the frame loop until the window closes or --frames is reached.
//
//   game_editor [--backend dx12|vulkan] [--project <folder>] [--workspace <file>] [--font <file>] [--frames <n>] [--debug-layers]
//
// The editor reads its shaders from the build's shader folder, so run it from a build that compiled them. A missing font falls back to
// ImGui's built-in font and is reported in the console; Cyrillic text needs the bundled Roboto or another font with that range.

#include "engine/core/file_io.hpp"
#include "engine/core/log.hpp"
#include "engine/core/path.hpp"
#include "engine/editor/console.hpp"
#include "engine/editor/editor_model.hpp"
#include "engine/editor/editor_ui.hpp"
#include "engine/editor/imgui_input.hpp"
#include "engine/editor/imgui_renderer.hpp"
#include "engine/platform/platform.hpp"
#include "engine/rhi/device.hpp"

#include <imgui.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#ifndef ENGINE_EDITOR_FONT_PATH
#define ENGINE_EDITOR_FONT_PATH ""
#endif
#ifndef ENGINE_EDITOR_SHADER_DIR
#define ENGINE_EDITOR_SHADER_DIR ""
#endif

namespace {

using namespace engine;

constexpr int kExitUsage = 2;
constexpr int kExitFailed = 1;
constexpr float kFontPixelSize = 18.0f;

struct Options {
    rhi::Backend backend = rhi::Backend::Dx12;
    std::string project;
    std::string workspace = "editor-workspace.json";
    std::string font = ENGINE_EDITOR_FONT_PATH;
    std::string shader_dir = ENGINE_EDITOR_SHADER_DIR;
    std::uint64_t frames = 0;  // 0 runs until the window closes
    bool debug_layers = false;
};

void print_usage() {
    std::puts("Usage: game_editor [--backend dx12|vulkan] [--project <folder>] [--workspace <file>] [--font <file>]");
    std::puts("                   [--frames <n>] [--debug-layers]");
    std::puts("");
    std::puts("Opens the game editor in a native window. --frames stops after n frames (for smoke runs).");
    std::puts("Exit codes: 0 closed normally, 1 failed to start or lost the device, 2 invalid command line.");
}

bool parse_options(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        const bool has_value = i + 1 < argc;
        if (arg == "-h" || arg == "--help") {
            print_usage();
            std::exit(0);
        } else if (arg == "--debug-layers") {
            options.debug_layers = true;
        } else if (arg == "--backend" && has_value) {
            const std::string_view value(argv[++i]);
            if (value == "dx12") {
                options.backend = rhi::Backend::Dx12;
            } else if (value == "vulkan") {
                options.backend = rhi::Backend::Vulkan;
            } else {
                return false;
            }
        } else if (arg == "--project" && has_value) {
            options.project = argv[++i];
        } else if (arg == "--workspace" && has_value) {
            options.workspace = argv[++i];
        } else if (arg == "--font" && has_value) {
            options.font = argv[++i];
        } else if (arg == "--frames" && has_value) {
            char* end = nullptr;
            const unsigned long long frames = std::strtoull(argv[++i], &end, 10);
            if (end == nullptr || *end != '\0' || frames == 0) {
                return false;
            }
            options.frames = frames;
        } else {
            return false;
        }
    }
    return true;
}

// The backend a window uses by default: DX12 on Windows, Vulkan on Linux.
rhi::Backend default_backend() {
#ifdef _WIN32
    return rhi::Backend::Dx12;
#else
    return rhi::Backend::Vulkan;
#endif
}

core::Result<std::vector<std::uint8_t>, std::string> read_bytes(const std::string& path_text) {
    auto path = core::Path::from_utf8(path_text);
    if (!path.ok()) {
        return core::failure(std::string("a shader path is not valid UTF-8"));
    }
    auto text = core::read_file(path.value());
    if (!text.ok()) {
        return core::failure(core::describe(text.error()));
    }
    const std::string& content = text.value();
    return core::success(std::vector<std::uint8_t>(content.begin(), content.end()));
}

// Loads the editor's compiled shaders for the backend. Their names carry the entry, and the extension is the backend's bytecode.
core::Result<editor::ImGuiShaders, std::string> load_shaders(const std::string& dir, rhi::Backend backend) {
    const std::string extension = backend == rhi::Backend::Vulkan ? ".spv" : ".dxil";
    auto vertex = read_bytes(dir + "/editor_imgui_vertex" + extension);
    if (!vertex.ok()) {
        return core::failure("the editor vertex shader: " + vertex.error());
    }
    auto pixel = read_bytes(dir + "/editor_imgui_pixel" + extension);
    if (!pixel.ok()) {
        return core::failure("the editor pixel shader: " + pixel.error());
    }
    editor::ImGuiShaders shaders;
    shaders.vertex = rhi::ShaderBinary{rhi::ShaderStage::Vertex, "VSMain", std::move(vertex).value()};
    shaders.pixel = rhi::ShaderBinary{rhi::ShaderStage::Pixel, "PSMain", std::move(pixel).value()};
    return core::success(std::move(shaders));
}

// Builds the font atlas. A font that cannot be read is reported and replaced by ImGui's built-in font, so the editor still starts.
void setup_font(ImGuiIO& io, const std::string& path, float dpi_scale, editor::Console& console) {
    const float pixel_size = kFontPixelSize * dpi_scale;
    if (path.empty() || io.Fonts->AddFontFromFileTTF(path.c_str(), pixel_size, nullptr, io.Fonts->GetGlyphRangesCyrillic()) == nullptr) {
        console.add(core::LogLevel::Warning, "font", "cannot load the font " + path + "; using the built-in font without Cyrillic");
        io.Fonts->Clear();
        io.Fonts->AddFontDefault();
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    options.backend = default_backend();
    if (!parse_options(argc, argv, options)) {
        print_usage();
        return kExitUsage;
    }

    auto console = std::make_shared<editor::Console>();
    core::add_log_sink(console);
    auto workspace_file = core::Path::from_utf8(options.workspace);
    if (!workspace_file.ok()) {
        core::log_message(core::LogLevel::Error, "editor", "the workspace path is not valid UTF-8");
        return kExitUsage;
    }
    editor::EditorModel model(console, workspace_file.value());
    model.startup(options.project);

    auto platform_result = platform::Platform::create();
    if (!platform_result.ok()) {
        core::log_message(core::LogLevel::Error, "editor", "the platform cannot start: " + platform_result.error().detail);
        return kExitFailed;
    }
    std::unique_ptr<platform::Platform> platform = std::move(platform_result).value();

    platform::WindowDesc window_desc;
    window_desc.title = "Game Editor";
    window_desc.size = platform::LogicalSize{1280.0, 720.0};
    auto window_result = platform->create_window(window_desc);
    if (!window_result.ok()) {
        core::log_message(core::LogLevel::Error, "editor", "cannot create the window: " + window_result.error().detail);
        return kExitFailed;
    }
    const platform::WindowId window = window_result.value();
    auto info_result = platform->window_info(window);
    if (!info_result.ok()) {
        return kExitFailed;
    }

    rhi::DeviceDesc device_desc;
    device_desc.backend = options.backend;
    device_desc.debug_layers = options.debug_layers;
    auto device_result = rhi::create_device(device_desc);
    if (!device_result.ok()) {
        core::log_message(core::LogLevel::Error, "editor", "cannot create the " + std::string(rhi::to_string(options.backend)) +
                                                              " device: " + device_result.error().message);
        return kExitFailed;
    }
    std::unique_ptr<rhi::Device> device = std::move(device_result).value();

    rhi::SwapchainDesc swapchain_desc;
    swapchain_desc.width = info_result.value().framebuffer_size.width;
    swapchain_desc.height = info_result.value().framebuffer_size.height;
#ifdef _WIN32
    // Win32 windows present through DX12 (hwnd) and through Vulkan's Win32 surface (hinstance and hwnd).
    auto handles = platform->win32_handles(window);
    if (!handles.ok()) {
        return kExitFailed;
    }
    swapchain_desc.hwnd = handles.value().hwnd;
    swapchain_desc.hinstance = handles.value().hinstance;
#else
    auto handles = platform->xlib_handles(window);
    if (!handles.ok()) {
        return kExitFailed;
    }
    swapchain_desc.x11_display = handles.value().display;
    swapchain_desc.x11_window = handles.value().window;
#endif
    auto swapchain_result = device->create_swapchain(swapchain_desc);
    if (!swapchain_result.ok()) {
        core::log_message(core::LogLevel::Error, "editor", "cannot create the swapchain: " + swapchain_result.error().message);
        return kExitFailed;
    }
    std::unique_ptr<rhi::Swapchain> swapchain = std::move(swapchain_result).value();

    auto shaders = load_shaders(options.shader_dir, options.backend);
    if (!shaders.ok()) {
        core::log_message(core::LogLevel::Error, "editor", "the editor cannot start without its shaders: " + shaders.error());
        return kExitFailed;
    }
    auto renderer_result = editor::ImGuiRenderer::create(*device, rhi::Format::B8G8R8A8Unorm, shaders.value());
    if (!renderer_result.ok()) {
        core::log_message(core::LogLevel::Error, "editor", "cannot create the UI renderer: " + renderer_result.error().message);
        return kExitFailed;
    }
    std::unique_ptr<editor::ImGuiRenderer> renderer = std::move(renderer_result).value();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // the workspace file holds the layout; ImGui does not write imgui.ini
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    // The font is rasterized at the window's DPI, so paddings and spacing scale with it.
    const float dpi_scale = static_cast<float>(info_result.value().dpi_scale);
    model.set_dpi_scale(info_result.value().dpi_scale);
    ImGui::GetStyle().ScaleAllSizes(dpi_scale);
    setup_font(io, options.font, dpi_scale, *console);
    unsigned char* atlas_pixels = nullptr;
    int atlas_width = 0;
    int atlas_height = 0;
    io.Fonts->GetTexDataAsRGBA32(&atlas_pixels, &atlas_width, &atlas_height);
    if (auto uploaded = renderer->upload_font_atlas(atlas_pixels, static_cast<std::uint32_t>(atlas_width),
                                                    static_cast<std::uint32_t>(atlas_height));
        !uploaded.ok()) {
        core::log_message(core::LogLevel::Error, "editor", "cannot upload the font atlas: " + uploaded.error().message);
        return kExitFailed;
    }

    editor::EditorUi ui(model);
    ui.apply_saved_layout();

    rhi::FenceHandle fence;
    if (auto created = device->create_fence(0); created.ok()) {
        fence = std::move(created).value();
    } else {
        return kExitFailed;
    }
    std::uint64_t fence_value = 0;
    const std::array<float, 4> clear{0.09f, 0.09f, 0.10f, 1.0f};

    using Clock = std::chrono::steady_clock;
    Clock::time_point previous = Clock::now();
    std::uint64_t frame = 0;
    int exit_code = 0;
    bool running = true;
    while (running && !model.quit_requested() && (options.frames == 0 || frame < options.frames)) {
        platform->pump_events();
        for (const platform::WindowEvent& event : platform->drain_events()) {
            if (std::holds_alternative<platform::CloseRequested>(event.data)) {
                // The window's close button asks first when the scene has unsaved changes.
                ui.request_exit();
            } else if (const auto* resized = std::get_if<platform::WindowResized>(&event.data)) {
                if (resized->framebuffer_size.width > 0 && resized->framebuffer_size.height > 0) {
                    static_cast<void>(swapchain->resize(resized->framebuffer_size.width, resized->framebuffer_size.height));
                }
            } else if (const auto* focus = std::get_if<platform::WindowFocusChanged>(&event.data)) {
                editor::set_imgui_focused(focus->focused);
            } else if (const auto* dpi = std::get_if<platform::WindowDpiChanged>(&event.data)) {
                // The viewport sizes its handles and pick radius by the DPI scale; the font and style keep their startup size.
                model.set_dpi_scale(dpi->dpi_scale);
            }
        }
        for (const platform::InputEvent& input : platform->drain_input_events()) {
            if (input.window == window) {
                editor::handle_imgui_input(input);
            }
        }

        auto current = platform->window_info(window);
        if (!current.ok()) {
            exit_code = kExitFailed;
            break;
        }
        const platform::WindowInfo& info = current.value();
        if (info.minimized || info.framebuffer_size.width == 0 || info.framebuffer_size.height == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        // The pointer is polled: it is inside the window only while the window has focus and the position is within its bounds.
        std::optional<platform::PointerPosition> pointer;
        if (info.focused) {
            if (auto position = platform->pointer_position(window); position.ok()) {
                const double width = static_cast<double>(info.framebuffer_size.width);
                const double height = static_cast<double>(info.framebuffer_size.height);
                if (position.value().x >= 0.0 && position.value().y >= 0.0 && position.value().x < width && position.value().y < height) {
                    pointer = position.value();
                }
            }
        }
        editor::set_imgui_pointer(pointer);

        const auto now = Clock::now();
        const float delta = std::chrono::duration<float>(now - previous).count();
        previous = now;
        editor::begin_imgui_frame(static_cast<float>(info.framebuffer_size.width), static_cast<float>(info.framebuffer_size.height), delta);
        ImGui::NewFrame();
        ui.build();
        ImGui::Render();

        auto back = swapchain->acquire_back_buffer();
        if (!back.ok()) {
            core::log_message(core::LogLevel::Error, "editor", "cannot acquire a back buffer: " + back.error().message);
            exit_code = kExitFailed;
            break;
        }
        if (!back.value().has_value()) {
            continue;  // the surface is empty; the next frame tries again
        }
        const rhi::TextureHandle target = *back.value();
        auto list_result = device->create_command_list(rhi::QueueType::Graphics);
        if (!list_result.ok()) {
            exit_code = kExitFailed;
            break;
        }
        std::unique_ptr<rhi::CommandList> list = std::move(list_result).value();
        // The frame's list is a single submission. Its fence is waited on before the next frame, so per-frame buffers can be reused.
        static_cast<void>(list->transition(target, rhi::ResourceState::Present, rhi::ResourceState::RenderTarget));
        if (auto drawn = renderer->record(*list, target, *ImGui::GetDrawData(), clear); !drawn.ok()) {
            core::log_message(core::LogLevel::Error, "editor", "the UI could not be drawn: " + drawn.error().message);
            exit_code = kExitFailed;
            break;
        }
        static_cast<void>(list->transition(target, rhi::ResourceState::RenderTarget, rhi::ResourceState::Present));
        static_cast<void>(list->close());
        ++fence_value;
        const std::vector<rhi::CommandList*> lists{list.get()};
        if (auto submitted = device->submit(rhi::QueueType::Graphics, lists, fence, fence_value); !submitted.ok()) {
            core::log_message(core::LogLevel::Error, "editor", "the frame could not be submitted: " + submitted.error().message);
            exit_code = kExitFailed;
            break;
        }
        if (auto waited = device->wait_fence(fence, fence_value); !waited.ok()) {
            exit_code = kExitFailed;
            break;
        }
        static_cast<void>(swapchain->present());
        ++frame;
    }

    // The layout is captured from ImGui and written with the rest of the workspace, so a closed editor reopens as it was left.
    ui.capture_layout();
    static_cast<void>(model.save_workspace());
    static_cast<void>(device->wait_idle());

    renderer.reset();
    swapchain.reset();
    ImGui::DestroyContext();
    static_cast<void>(device->destroy(fence));
    device.reset();
    static_cast<void>(platform->destroy_window(window));
    core::remove_log_sink(console);
    return exit_code;
}
