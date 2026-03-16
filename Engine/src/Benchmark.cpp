#include <Debug/Benchmark.hpp>
#include <Graphics/Renderer.hpp>
#include <Debug/SystemMetrics.hpp>
#include <Logger.hpp>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <cmath>
#include <cfloat>

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

    // Reset stats
    m_minFPS = INT_MAX;
    m_maxFPS = 0;
    m_minFrameTime = FLT_MAX;
    m_maxFrameTime = 0.0f;
    m_sumFPS = 0.0;
    m_sumFrameTime = 0.0;
    m_sumVertices = 0.0;
    m_sumRAM = 0.0;
    m_customSums.assign(m_customMetrics.size(), 0.0);

    // Write CSV header
    m_file << "Frame,Time,FPS,FrameTime_ms,Vertices,RAM_MB";
    for (auto& metric : m_customMetrics)
        m_file << "," << metric.Name;
    m_file << "\n";

    SLEAK_INFO("Benchmark: Recording started -> benchmarks/{}", filename);
}

void Benchmark::StopRecording() {
    if (!m_recording) return;

    m_recording = false;

    WriteSummary();
    m_file.close();

    SLEAK_INFO("Benchmark: Recording stopped ({} frames captured)", m_frameIndex);
}

void Benchmark::WriteFrameData(float deltaTime) {
    if (!m_file.is_open() || !m_renderer) return;

    float elapsed = m_sessionTimer.Elapsed();
    int fps = m_renderer->GetFrameRate();
    float frameTimeMs = deltaTime * 1000.0f;
    int vertices = m_renderer->GetVertices();

    auto sysMetrics = SystemMetrics::Query();
    float ramMB = sysMetrics.RamUsageMB;

    // Accumulate stats (skip frame 0 which may have setup cost)
    if (m_frameIndex > 0) {
        if (fps < m_minFPS) m_minFPS = fps;
        if (fps > m_maxFPS) m_maxFPS = fps;
        if (frameTimeMs < m_minFrameTime) m_minFrameTime = frameTimeMs;
        if (frameTimeMs > m_maxFrameTime) m_maxFrameTime = frameTimeMs;
        m_sumFPS += fps;
        m_sumFrameTime += frameTimeMs;
        m_sumVertices += vertices;
        m_sumRAM += ramMB;
    }

    m_file << m_frameIndex
           << "," << std::fixed << std::setprecision(4) << elapsed
           << "," << fps
           << "," << std::setprecision(3) << frameTimeMs
           << "," << vertices
           << "," << std::setprecision(1) << ramMB;

    for (size_t i = 0; i < m_customMetrics.size(); ++i) {
        float val = m_customMetrics[i].Getter();
        m_file << "," << std::setprecision(2) << val;
        if (m_frameIndex > 0 && i < m_customSums.size())
            m_customSums[i] += val;
    }

    m_file << "\n";
    m_frameIndex++;
}

void Benchmark::WriteSummary() {
    if (m_frameIndex <= 1) return;
    int n = m_frameIndex - 1; // exclude frame 0

    m_file << "\n# Summary\n";
    m_file << "# Frames," << m_frameIndex << "\n";
    m_file << "# Duration_s," << std::fixed << std::setprecision(2) << m_sessionTimer.Elapsed() << "\n";
    m_file << "# Renderer," << GetRendererTag() << "\n";

    m_file << "# FPS_Min," << m_minFPS << "\n";
    m_file << "# FPS_Max," << m_maxFPS << "\n";
    m_file << "# FPS_Avg," << std::setprecision(1) << (m_sumFPS / n) << "\n";

    m_file << "# FrameTime_Min_ms," << std::setprecision(3) << m_minFrameTime << "\n";
    m_file << "# FrameTime_Max_ms," << std::setprecision(3) << m_maxFrameTime << "\n";
    m_file << "# FrameTime_Avg_ms," << std::setprecision(3) << (m_sumFrameTime / n) << "\n";

    m_file << "# Vertices_Avg," << std::setprecision(0) << (m_sumVertices / n) << "\n";
    m_file << "# RAM_Avg_MB," << std::setprecision(1) << (m_sumRAM / n) << "\n";

    for (size_t i = 0; i < m_customMetrics.size() && i < m_customSums.size(); ++i) {
        m_file << "# " << m_customMetrics[i].Name << "_Avg,"
               << std::setprecision(2) << (m_customSums[i] / n) << "\n";
    }
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
    auto now = std::chrono::system_clock::now();
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
