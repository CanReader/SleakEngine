#pragma once

#include <Core/OSDef.hpp>

namespace Sleak {

struct SystemMetricsData {
    float CpuUsagePercent = 0.0f;
    float RamUsageMB = 0.0f;
    float GpuUsagePercent = 0.0f;
    float GpuMemoryUsedMB = 0.0f;
};

class ENGINE_API SystemMetrics {
public:
    static void Initialize();
    static void Shutdown();
    static SystemMetricsData Query();

private:
    static bool s_initialized;
};

}  // namespace Sleak
