#include "engine/runtime/windowed_game.hpp"

#include "engine/platform/platform.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/swapchain.hpp"
#include "engine/gameplay/components.hpp"
#include "engine/runtime/game_hud.hpp"
#include "engine/runtime/game_renderer.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <memory>
#include <set>
#include <thread>
#include <variant>
#include <vector>

namespace engine::runtime {

namespace {

int failed(const std::string& message) {
    std::fprintf(stderr, "game_runtime: %s\n", message.c_str());
    return 1;
}

}  // namespace

int run_windowed_game(scene::Scene& scene, gameplay::GameWorld& world, const RuntimeOptions& options, const std::string& shader_dir,
                      AssetReader reader, const std::map<core::Guid, std::string>& animator_names) {
    auto platform_result = platform::Platform::create();
    if (!platform_result.ok()) {
        return failed("the platform cannot start: " + platform_result.error().detail);
    }
    std::unique_ptr<platform::Platform> platform = std::move(platform_result).value();

    platform::WindowDesc window_desc;
    window_desc.title = "Game";
    window_desc.size = platform::LogicalSize{1280.0, 720.0};
    window_desc.resizable = false;
    auto window_result = platform->create_window(window_desc);
    if (!window_result.ok()) {
        return failed("cannot create the window: " + window_result.error().detail);
    }
    const platform::WindowId window = window_result.value();
    auto info_result = platform->window_info(window);
    if (!info_result.ok()) {
        return failed("cannot read the window size");
    }
    const std::uint32_t fb_width = info_result.value().framebuffer_size.width;
    const std::uint32_t fb_height = info_result.value().framebuffer_size.height;

    rhi::DeviceDesc device_desc;
    device_desc.backend = options.backend == kBackendVulkan ? rhi::Backend::Vulkan : rhi::Backend::Dx12;
    auto device_result = rhi::create_device(device_desc);
    if (!device_result.ok()) {
        return failed("cannot create the " + options.backend + " device: " + device_result.error().message);
    }
    std::unique_ptr<rhi::Device> device = std::move(device_result).value();

    rhi::SwapchainDesc swapchain_desc;
    swapchain_desc.width = fb_width;
    swapchain_desc.height = fb_height;
#ifdef _WIN32
    auto handles = platform->win32_handles(window);
    if (!handles.ok()) {
        return failed("cannot read the window handles");
    }
    swapchain_desc.hwnd = handles.value().hwnd;
    swapchain_desc.hinstance = handles.value().hinstance;
#else
    auto handles = platform->xlib_handles(window);
    if (!handles.ok()) {
        return failed("cannot read the window handles");
    }
    swapchain_desc.x11_display = handles.value().display;
    swapchain_desc.x11_window = handles.value().window;
#endif
    auto swapchain_result = device->create_swapchain(swapchain_desc);
    if (!swapchain_result.ok()) {
        return failed("cannot create the swapchain: " + swapchain_result.error().message);
    }
    std::unique_ptr<rhi::Swapchain> swapchain = std::move(swapchain_result).value();

    // The renderer's width is a multiple of 64 so its readback rows are tight; the picture is placed at the window's top-left corner.
    const std::uint32_t render_width = std::max<std::uint32_t>(64, fb_width / 64 * 64);
    auto renderer_result = GameRenderer::create(*device, device_desc.backend, shader_dir, render_width, fb_height, reader);
    if (!renderer_result.ok()) {
        return failed(renderer_result.error());
    }
    const AssetReader reader_copy = reader;
    std::unique_ptr<GameRenderer> renderer = std::move(renderer_result).value();
    for (const std::string& problem : renderer->bind_scene(scene, animator_names)) {
        std::fprintf(stderr, "game_runtime: %s\n", problem.c_str());
    }

    // The HUD: the first visible UI canvas. Its font ships beside the shaders; a game without one still runs, without its UI.
    std::unique_ptr<GameHud> hud;
    {
        for (const core::Guid& guid : scene.entity_guids()) {
            const auto types = scene.component_types(guid);
            if (std::find(types.begin(), types.end(), gameplay::kUiCanvasId) == types.end()) {
                continue;
            }
            const auto visible = scene.field(guid, gameplay::kUiCanvasId, "visible");
            if (visible && std::holds_alternative<bool>(*visible) && !std::get<bool>(*visible)) {
                continue;
            }
            const auto asset_text = [&](const char* field) -> std::string {
                const auto value = scene.field(guid, gameplay::kUiCanvasId, field);
                if (value && std::holds_alternative<core::Guid>(*value) && !std::get<core::Guid>(*value).is_nil() && reader_copy) {
                    return reader_copy(std::get<core::Guid>(*value)).value_or(std::string());
                }
                return {};
            };
            const std::string layout_text = asset_text("layout");
            if (layout_text.empty()) {
                continue;
            }
            const std::filesystem::path font_path = std::filesystem::path(shader_dir).parent_path() / "fonts" / "ui.ttf";
            std::ifstream font_file(font_path, std::ios::binary);
            const std::vector<std::uint8_t> font_bytes((std::istreambuf_iterator<char>(font_file)), std::istreambuf_iterator<char>());
            if (font_bytes.empty()) {
                std::fprintf(stderr, "game_runtime: the UI needs %s, which is missing; the game runs without its UI\n", font_path.string().c_str());
                break;
            }
            auto created = GameHud::create(world.scripts(), layout_text, asset_text("script"), font_bytes, fb_width, fb_height);
            if (!created.ok()) {
                std::fprintf(stderr, "game_runtime: %s\n", created.error().c_str());
                break;
            }
            hud = std::move(created).value();
            break;
        }
    }

    std::set<platform::Key> held;  // keys down now: WASD or the arrows walk the player, Space jumps

    using Clock = std::chrono::steady_clock;
    Clock::time_point previous = Clock::now();
    std::uint64_t frame = 0;
    int exit_code = 0;
    bool running = true;
    while (running && (!options.frames_given || frame < options.frames)) {
        platform->pump_events();
        for (const platform::WindowEvent& event : platform->drain_events()) {
            if (std::holds_alternative<platform::CloseRequested>(event.data)) {
                running = false;
            }
        }
        const std::vector<platform::InputEvent> inputs = platform->drain_input_events();
        bool jump = false;
        for (const platform::InputEvent& input : inputs) {
            if (const auto* pressed = std::get_if<platform::KeyPressed>(&input.data)) {
                held.insert(pressed->key);
                jump = jump || pressed->key == platform::Key::Space;
            } else if (const auto* released = std::get_if<platform::KeyReleased>(&input.data)) {
                held.erase(released->key);
            }
            if (const auto* key = std::get_if<platform::KeyPressed>(&input.data); key != nullptr && key->key == platform::Key::Escape) {
                running = false;
            }
        }
        auto current = platform->window_info(window);
        if (!current.ok()) {
            exit_code = 1;
            break;
        }
        if (current.value().minimized) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        const Clock::time_point now = Clock::now();
        // A long pause (a drag of the window, a breakpoint) is not simulated in one step.
        const double delta = std::min(std::chrono::duration<double>(now - previous).count(), 0.1);
        previous = now;
        const auto down = [&held](platform::Key a, platform::Key b) { return held.count(a) > 0 || held.count(b) > 0; };
        world.set_player_input(glm::vec2((down(platform::Key::D, platform::Key::Right) ? 1.0f : 0.0f) - (down(platform::Key::A, platform::Key::Left) ? 1.0f : 0.0f),
                                         (down(platform::Key::W, platform::Key::Up) ? 1.0f : 0.0f) - (down(platform::Key::S, platform::Key::Down) ? 1.0f : 0.0f)),
                               jump);
        world.advance(delta);
        for (const std::string& error : world.take_errors()) {
            std::fprintf(stderr, "game_runtime: %s\n", error.c_str());
        }

        if (hud != nullptr) {
            float pointer_x = -1.0f;
            float pointer_y = -1.0f;
            if (current.value().focused) {
                if (auto pointer = platform->pointer_position(window); pointer.ok()) {
                    pointer_x = static_cast<float>(pointer.value().x);
                    pointer_y = static_cast<float>(pointer.value().y);
                }
            }
            hud->update(pointer_x, pointer_y, inputs);
            renderer->set_overlay(hud->render());
        }
        auto drawn = renderer->render(scene, &world.animators(), delta);
        if (!drawn.ok()) {
            exit_code = failed(drawn.error());
            break;
        }
        auto back = swapchain->acquire_back_buffer();
        if (!back.ok()) {
            exit_code = failed("cannot acquire a back buffer: " + back.error().message);
            break;
        }
        if (!back.value().has_value()) {
            continue;  // the surface is empty; the next frame tries again
        }
        if (auto presented = renderer->present(*back.value(), fb_width, fb_height); !presented.ok()) {
            exit_code = failed(presented.error());
            break;
        }
        static_cast<void>(swapchain->present());
        ++frame;
    }

    static_cast<void>(device->wait_idle());
    renderer.reset();
    swapchain.reset();
    device.reset();
    static_cast<void>(platform->destroy_window(window));
    std::printf("game_runtime: drew %llu frames\n", static_cast<unsigned long long>(frame));
    return exit_code;
}

}  // namespace engine::runtime
