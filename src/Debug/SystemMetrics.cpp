#include <Debug/SystemMetrics.hpp>

#ifdef PLATFORM_WIN
#include <windows.h>
#include <psapi.h>
#include <pdh.h>
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "psapi.lib")
#endif

#ifdef PLATFORM_LINUX
#include <cstdio>
#include <cstring>
#endif

namespace Sleak {

bool SystemMetrics::s_initialized = false;

#ifdef PLATFORM_WIN

static PDH_HQUERY s_cpuQuery = nullptr;
static PDH_HCOUNTER s_cpuCounter = nullptr;

void SystemMetrics::Initialize() {
    if (s_initialized) return;

    PdhOpenQuery(nullptr, 0, &s_cpuQuery);
    PdhAddEnglishCounterA(s_cpuQuery,
                          "\\Processor(_Total)\\% Processor Time",
                          0, &s_cpuCounter);
    PdhCollectQueryData(s_cpuQuery);

    s_initialized = true;
}

void SystemMetrics::Shutdown() {
    if (!s_initialized) return;

    if (s_cpuQuery) {
        PdhCloseQuery(s_cpuQuery);
        s_cpuQuery = nullptr;
        s_cpuCounter = nullptr;
    }

    s_initialized = false;
}

SystemMetricsData SystemMetrics::Query() {
    SystemMetricsData data;
    if (!s_initialized) return data;

    // CPU usage
    PdhCollectQueryData(s_cpuQuery);
    PDH_FMT_COUNTERVALUE counterVal;
    PdhGetFormattedCounterValue(s_cpuCounter, PDH_FMT_DOUBLE,
                                nullptr, &counterVal);
    data.CpuUsagePercent = static_cast<float>(counterVal.doubleValue);

    // RAM usage (process working set)
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                             sizeof(pmc))) {
        data.RamUsageMB =
            static_cast<float>(pmc.WorkingSetSize) / (1024.0f * 1024.0f);
    }

    return data;
}

#elif defined(PLATFORM_LINUX)

void SystemMetrics::Initialize() {
    s_initialized = true;
}

void SystemMetrics::Shutdown() {
    s_initialized = false;
}

SystemMetricsData SystemMetrics::Query() {
    SystemMetricsData data;
    if (!s_initialized) return data;

    // RAM usage from /proc/self/status
    FILE* f = fopen("/proc/self/status", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "VmRSS:", 6) == 0) {
                long kb = 0;
                sscanf(line + 6, "%ld", &kb);
                data.RamUsageMB = static_cast<float>(kb) / 1024.0f;
                break;
            }
        }
        fclose(f);
    }

    return data;
}

#else

void SystemMetrics::Initialize() {
    s_initialized = true;
}

void SystemMetrics::Shutdown() {
    s_initialized = false;
}

SystemMetricsData SystemMetrics::Query() {
    return SystemMetricsData{};
}

#endif

}  // namespace Sleak
