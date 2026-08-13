#include <Debug/Benchmark.hpp>
#include <Graphics/Common/Renderer.hpp>
#include <Debug/SystemMetrics.hpp>
#include <Core/Logger.hpp>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <cmath>
#include <cfloat>
#include <numeric>

#ifdef PLATFORM_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#include <winreg.h>
#pragma comment(lib, "dxgi.lib")
#endif

using namespace Sleak;

Benchmark::~Benchmark() {
    if (m_recording)
        StopRecording();
}

void Benchmark::Initialize(RenderEngine::Renderer* renderer) {
    m_renderer = renderer;
}

void Benchmark::RegisterMetric(const std::string& name, std::function<float()> getter) {
    for (auto& m : m_customMetrics) {
        if (m.Name == name) {
            m.Getter = std::move(getter);
            return;
        }
    }
    m_customMetrics.push_back({name, std::move(getter)});
}

void Benchmark::UnregisterMetric(const std::string& name) {
    m_customMetrics.erase(
        std::remove_if(m_customMetrics.begin(), m_customMetrics.end(),
            [&](const BenchmarkMetric& m) { return m.Name == name; }),
        m_customMetrics.end());
}

void Benchmark::Tick(float deltaTime) {
    if (!m_recording) return;
    WriteFrameData(deltaTime);
}

void Benchmark::ToggleRecording() {
    if (m_recording)
        StopRecording();
    else
        StartRecording();
}

void Benchmark::StartRecording() {
    if (m_recording) return;

    std::string filename = GenerateFilename();
    std::filesystem::create_directories("benchmarks");

    m_file.open("benchmarks/" + filename);
    if (!m_file.is_open()) {
        SLEAK_WARN("Benchmark: Failed to open file: benchmarks/{}", filename);
        return;
    }

    m_recording = true;
    m_frameIndex = 0;
    m_sessionTimer.Reset();

    // Reset accumulators
    m_minFPS = INT_MAX;
    m_maxFPS = 0;
    m_sumFPS = 0.0;
    m_minFrameTime = FLT_MAX;
    m_maxFrameTime = 0.0f;
    m_sumFrameTime = 0.0;
    m_sumTriangles = 0.0;
    m_sumCPU = 0.0;
    m_sumRAM = 0.0;
    m_spikes16 = 0;
    m_spikes33 = 0;
    m_spikes50 = 0;
    m_frameTimes.clear();
    m_customSums.assign(m_customMetrics.size(), 0.0);

    // CSV header
    m_file << "Frame,Time_s,FrameTime_ms,FPS,Triangles,CPU_%,RAM_MB";
    for (auto& metric : m_customMetrics)
        m_file << "," << metric.Name;
    m_file << "\n";

    SLEAK_INFO("Benchmark: Recording started -> benchmarks/{}", filename);
}

void Benchmark::StopRecording() {
    if (!m_recording) return;

    m_recording = false;

    WriteSummary();
    WriteHardwareInfo();
    m_file.close();

    SLEAK_INFO("Benchmark: Recording stopped ({} frames captured)", m_frameIndex);
}

void Benchmark::WriteFrameData(float deltaTime) {
    if (!m_file.is_open() || !m_renderer) return;

    float elapsed      = m_sessionTimer.Elapsed();
    float frameTimeMs  = deltaTime * 1000.0f;
    int   fps          = m_renderer->GetFrameRate();
    int   triangles    = m_renderer->GetTriangles();

    auto  sysMetrics   = SystemMetrics::Query();
    float cpuPct       = sysMetrics.CpuUsagePercent;
    float ramMB        = sysMetrics.RamUsageMB;

    // Accumulate stats (skip frame 0 — may include setup cost)
    if (m_frameIndex > 0) {
        if (fps < m_minFPS)          m_minFPS = fps;
        if (fps > m_maxFPS)          m_maxFPS = fps;
        if (frameTimeMs < m_minFrameTime) m_minFrameTime = frameTimeMs;
        if (frameTimeMs > m_maxFrameTime) m_maxFrameTime = frameTimeMs;

        m_sumFPS       += fps;
        m_sumFrameTime += frameTimeMs;
        m_sumTriangles += triangles;
        m_sumCPU       += cpuPct;
        m_sumRAM       += ramMB;

        m_frameTimes.push_back(frameTimeMs);

        if (frameTimeMs > 16.67f) ++m_spikes16;
        if (frameTimeMs > 33.33f) ++m_spikes33;
        if (frameTimeMs > 50.0f)  ++m_spikes50;
    }

    m_file << m_frameIndex
           << "," << std::fixed << std::setprecision(4) << elapsed
           << "," << std::setprecision(3) << frameTimeMs
           << "," << fps
           << "," << triangles
           << "," << std::setprecision(1) << cpuPct
           << "," << std::setprecision(1) << ramMB;

    for (size_t i = 0; i < m_customMetrics.size(); ++i) {
        float val = m_customMetrics[i].Getter();
        m_file << "," << std::setprecision(2) << val;
        if (m_frameIndex > 0 && i < m_customSums.size())
            m_customSums[i] += val;
    }

    m_file << "\n";
    ++m_frameIndex;
}

void Benchmark::WriteSummary() {
    if (m_frameIndex <= 1) return;
    int n = m_frameIndex - 1;  // exclude frame 0

    // Percentiles — sort a copy of collected frame times
    std::vector<float> sorted = m_frameTimes;
    std::sort(sorted.begin(), sorted.end());
    auto percentile = [&](float p) -> float {
        if (sorted.empty()) return 0.0f;
        size_t idx = static_cast<size_t>(p * 0.01f * (sorted.size() - 1));
        return sorted[std::min(idx, sorted.size() - 1)];
    };

    // Frame time standard deviation (jitter metric)
    double avgFT = m_sumFrameTime / n;
    double variance = 0.0;
    for (float ft : m_frameTimes)
        variance += (ft - avgFT) * (ft - avgFT);
    float stdev = static_cast<float>(std::sqrt(variance / m_frameTimes.size()));

    m_file << "\n# Summary\n";
    m_file << "# Frames,"       << m_frameIndex << "\n";
    m_file << "# Duration_s,"   << std::fixed << std::setprecision(2) << m_sessionTimer.Elapsed() << "\n";
    m_file << "# Renderer,"     << GetRendererTag() << "\n";

    if (m_renderer) {
        m_file << "# VSync,"    << (m_renderer->GetVSync() ? "On" : "Off") << "\n";
        m_file << "# MSAA,"     << m_renderer->GetMSAASampleCount() << "x\n";
    }

    m_file << "#\n";
    m_file << "# FPS_Min,"      << m_minFPS << "\n";
    m_file << "# FPS_Max,"      << m_maxFPS << "\n";
    m_file << "# FPS_Avg,"      << std::setprecision(1) << (m_sumFPS / n) << "\n";

    m_file << "#\n";
    m_file << "# FrameTime_Min_ms," << std::setprecision(3) << m_minFrameTime << "\n";
    m_file << "# FrameTime_Max_ms," << std::setprecision(3) << m_maxFrameTime << "\n";
    m_file << "# FrameTime_Avg_ms," << std::setprecision(3) << avgFT << "\n";
    m_file << "# FrameTime_P50_ms," << std::setprecision(3) << percentile(50.0f) << "\n";
    m_file << "# FrameTime_P95_ms," << std::setprecision(3) << percentile(95.0f) << "\n";
    m_file << "# FrameTime_P99_ms," << std::setprecision(3) << percentile(99.0f) << "\n";
    m_file << "# FrameTime_Stdev_ms," << std::setprecision(3) << stdev << "\n";

    m_file << "#\n";
    m_file << "# Spikes_16ms," << m_spikes16 << "\n";
    m_file << "# Spikes_33ms," << m_spikes33 << "\n";
    m_file << "# Spikes_50ms," << m_spikes50 << "\n";

    m_file << "#\n";
    m_file << "# Triangles_Avg," << std::setprecision(0) << (m_sumTriangles / n) << "\n";
    m_file << "# CPU_Avg_%,"     << std::setprecision(1) << (m_sumCPU / n) << "\n";
    m_file << "# RAM_Avg_MB,"    << std::setprecision(1) << (m_sumRAM / n) << "\n";

    for (size_t i = 0; i < m_customMetrics.size() && i < m_customSums.size(); ++i) {
        m_file << "# " << m_customMetrics[i].Name << "_Avg,"
               << std::setprecision(2) << (m_customSums[i] / n) << "\n";
    }
}

void Benchmark::WriteHardwareInfo() {
    m_file << "#\n";
    m_file << "# --- System Info ---\n";

#ifdef PLATFORM_WIN
    // GPU name + dedicated VRAM via DXGI
    std::string gpuName  = "Unknown";
    float       vramGB   = 0.0f;
    {
        IDXGIFactory* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory)))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(factory->EnumAdapters(0, &adapter))) {
                DXGI_ADAPTER_DESC desc{};
                if (SUCCEEDED(adapter->GetDesc(&desc))) {
                    char narrow[256]{};
                    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                                        narrow, sizeof(narrow), nullptr, nullptr);
                    gpuName = narrow;
                    vramGB  = static_cast<float>(desc.DedicatedVideoMemory)
                              / (1024.0f * 1024.0f * 1024.0f);
                }
                adapter->Release();
            }
            factory->Release();
        }
    }

    // CPU name from registry
    std::string cpuName = "Unknown";
    {
        char buf[256]{};
        DWORD size = sizeof(buf);
        if (RegGetValueA(HKEY_LOCAL_MACHINE,
                         "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                         "ProcessorNameString",
                         RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) {
            cpuName = buf;
            // Trim trailing spaces
            while (!cpuName.empty() && cpuName.back() == ' ')
                cpuName.pop_back();
        }
    }

    // Logical CPU count
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    int cpuCores = static_cast<int>(si.dwNumberOfProcessors);

    // Total installed RAM
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    float totalRamGB = static_cast<float>(ms.ullTotalPhys) / (1024.0f * 1024.0f * 1024.0f);

    // OS version via RtlGetVersion (avoids deprecated GetVersionEx)
    std::string osVersion = "Windows";
    {
        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (ntdll) {
            auto fn = reinterpret_cast<RtlGetVersionFn>(
                GetProcAddress(ntdll, "RtlGetVersion"));
            if (fn) {
                RTL_OSVERSIONINFOW rovi{};
                rovi.dwOSVersionInfoSize = sizeof(rovi);
                if (fn(&rovi) == 0) {
                    std::ostringstream oss;
                    oss << "Windows " << rovi.dwMajorVersion
                        << "." << rovi.dwMinorVersion
                        << " (Build " << rovi.dwBuildNumber << ")";
                    osVersion = oss.str();
                }
            }
        }
    }

    m_file << "# CPU,"          << cpuName  << "\n";
    m_file << "# CPU_Cores,"    << cpuCores << "\n";
    m_file << "# Total_RAM_GB," << std::fixed << std::setprecision(1) << totalRamGB << "\n";
    m_file << "# GPU,"          << gpuName  << "\n";
    m_file << "# GPU_VRAM_GB,"  << std::setprecision(1) << vramGB << "\n";
    m_file << "# OS,"           << osVersion << "\n";
#else
    m_file << "# CPU,N/A\n";
    m_file << "# GPU,N/A\n";
#endif
}

std::string Benchmark::GetRendererTag() const {
    if (!m_renderer) return "unknown";
    switch (m_renderer->GetType()) {
        case RenderEngine::RendererType::DirectX11: return "dx11";
        case RenderEngine::RendererType::DirectX12: return "dx12";
        case RenderEngine::RendererType::OpenGL:    return "opengl";
        case RenderEngine::RendererType::Vulkan:    return "vulkan";
        default: return "unknown";
    }
}

std::string Benchmark::GenerateFilename() const {
    auto now  = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef PLATFORM_WIN
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif

    std::ostringstream oss;
    oss << "benchmark_" << GetRendererTag() << "_"
        << std::put_time(&tm, "%Y%m%d_%H%M%S") << ".csv";
    return oss.str();
}
