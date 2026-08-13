#pragma once

#include <Core/OSDef.hpp>

namespace Sleak {

/// Snapshot of OS-reported CPU/RAM/GPU usage at the moment of the last Query.
struct SystemMetricsData {
    float CpuUsagePercent = 0.0f;
    float RamUsageMB = 0.0f;
    float GpuUsagePercent = 0.0f;
    float GpuMemoryUsedMB = 0.0f;
};

/// Polls platform-specific counters (PDH on Windows, /proc on Linux) for CPU/RAM/GPU usage.
class ENGINE_API SystemMetrics {
public:
    static void Initialize();
    static void Shutdown();
    /// Reads the current CPU/RAM/GPU usage from the OS.
    static SystemMetricsData Query();

private:
    static bool s_initialized;
};

}  // namespace Sleak
