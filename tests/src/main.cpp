#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>

#include "cli_options.hpp"
#include "gpu_test_options.hpp"

#include "engine/platform/platform.hpp"
#include "engine/rhi/device.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Why the named GPU backend cannot run here, or empty when it can. A missing loader, driver, display, or adapter is "unsupported"
// (exit 3), which is a different result from a failing test. Any other creation error is left to the tests to report.
std::string gpu_backend_unavailable(std::string_view backend) {
    if (backend != "dx12" && backend != "vulkan") {
        return "unknown backend";
    }
#ifndef _WIN32
    if (backend == "dx12") {
        return "the dx12 backend exists only on Windows";
    }
#endif

    const auto platform = engine::platform::Platform::create();
    if (!platform.ok()) {
        return "no display for windows (" + platform.error().detail + ")";
    }

    engine::rhi::DeviceDesc desc;
    desc.backend = backend == "dx12" ? engine::rhi::Backend::Dx12 : engine::rhi::Backend::Vulkan;
    desc.debug_layers = true;
    desc.allow_software_adapter = engine::tests::allow_software_adapters();
    const auto probe = engine::rhi::create_device(desc);
    if (!probe.ok() && probe.error().code == engine::rhi::RhiErrorCode::Unsupported) {
        return probe.error().message;
    }
    return {};
}

}  // namespace

int main(int argc, char* argv[]) {
    constexpr int kExitUsageError = 2;
    constexpr int kExitUnsupported = 3;
    constexpr int kExitFailed = 1;

    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    const auto parsed = engine::tests::parse_command_line(args);
    if (!parsed.error.empty()) {
        std::cerr << "engine_tests: " << parsed.error << "\n\n" << engine::tests::usage_text();
        return kExitUsageError;
    }
    if (parsed.options.show_help) {
        std::cout << engine::tests::usage_text();
        return EXIT_SUCCESS;
    }

    // The editor suite runs on the GPU only when a backend is named; its GPU tests are then reported like the gpu suite.
    const bool gpu = parsed.options.suite == engine::tests::Suite::Gpu ||
                     (parsed.options.suite == engine::tests::Suite::Editor && !parsed.options.backend.empty());
    if (gpu) {
        const std::string reason = gpu_backend_unavailable(parsed.options.backend);
        if (!reason.empty()) {
            std::cout << "engine_tests: unsupported: " << reason << " (not run)\n";
            return kExitUnsupported;
        }
        if (engine::tests::allow_software_adapters()) {
            std::cout << "engine_tests: ENGINE_GPU_ALLOW_SOFTWARE=1: software adapters may run; this is not a GPU report\n";
        }
    }

    Catch::Session session;
    session.configData().testsOrTags.emplace_back(engine::tests::suite_tag_expression(parsed.options.suite, parsed.options.backend));
    const int failures = session.run();
    // A GPU run reports any failure as 1, so a count of 3 cannot be mistaken for the unsupported exit code.
    if (gpu && failures != 0) {
        return kExitFailed;
    }
    return failures;
}
