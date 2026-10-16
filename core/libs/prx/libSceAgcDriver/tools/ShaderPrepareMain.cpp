#include "ShaderPrepare.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include <csignal>
#include <cstdio>
#include <exception>
#include <memory>
#include <string_view>
#include <vector>

namespace {

void cancel(int) {
    AgcDriver::ShaderPreparationTool::Cancel();
}

}

int main(int argc, char** argv) {
    using namespace AgcDriver::ShaderPreparationTool;
    try {
        std::vector<std::string_view> arguments;
        for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
        const auto options = ParseOptions(arguments);
        if (options.help) {
            PrintUsage();
            return 0;
        }
        std::signal(SIGINT, cancel);
        std::signal(SIGTERM, cancel);
        ConfigureOutput(options);
        std::unique_ptr<AgcDriver::VulkanDevice> device;
        if (options.target == TargetSelection::Current) {
            device = std::make_unique<AgcDriver::VulkanDevice>(AgcDriver::VulkanDevice::CompileOnly{});
            std::printf("[shader-prepare] target=current device=%s driver=%s\n", device->DeviceName().c_str(), device->DriverInfo().c_str());
        } else std::puts("[shader-prepare] target=recorded; native readiness requires explicit current-target recipe preparation");
        const auto summary = Run(options, device.get());
        return summary.cancelled ? 130 : summary.Complete() ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[shader-prepare] %s\n", error.what());
        return 2;
    }
}
