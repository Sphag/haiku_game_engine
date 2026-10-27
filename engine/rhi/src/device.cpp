#include "engine/rhi/device.hpp"

#include "engine/rhi/validation_device.hpp"

#ifdef _WIN32
#include "dx12/dx12_device.hpp"
#endif
#include "vulkan/vulkan_device.hpp"

#include <utility>

namespace engine::rhi {

RhiResult<std::unique_ptr<Device>> create_device(const DeviceDesc& desc) {
    switch (desc.backend) {
        case Backend::Validation: {
            ValidationDeviceDesc validation;
            validation.disabled_features = desc.disabled_features;
            std::unique_ptr<Device> device = std::make_unique<ValidationDevice>(validation);
            return core::success(std::move(device));
        }
        case Backend::Dx12:
#ifdef _WIN32
            return dx12::create_dx12_device(desc);
#else
            return fail<std::unique_ptr<Device>>(RhiErrorCode::Unsupported, "the dx12 backend exists only on Windows");
#endif
        case Backend::Vulkan:
            return vulkan::create_vulkan_device(desc);
    }
    return fail<std::unique_ptr<Device>>(RhiErrorCode::Unsupported, "unknown backend");
}

}  // namespace engine::rhi
