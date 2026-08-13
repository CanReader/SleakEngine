#pragma once

#include <Core/OSDef.hpp>
#include <Core/Timer.hpp>
#include <string>
#include <vector>
#include <functional>
#include <fstream>

namespace Sleak {

namespace RenderEngine { class Renderer; }

// A custom metric that the game module (or anyone) can register
struct BenchmarkMetric {
    std::string Name;
    std::function<float()> Getter;
};

class ENGINE_API Benchmark {
public:
    Benchmark() = default;
    ~Benchmark();

    void Initialize(RenderEngine::Renderer* renderer);

    // Register a custom metric (e.g. render distance from Game)
    void RegisterMetric(const std::string& name, std::function<float()> getter);
    void UnregisterMetric(const std::string& name);

    // Call once per frame from the main loop
    void Tick(float deltaTime);

    // Toggle recording on/off (F12)
    void ToggleRecording();
    bool IsRecording() const { return m_recording; }

private:
    void StartRecording();
    void StopRecording();
    void WriteFrameData(float deltaTime);
    void WriteSummary();
    void WriteHardwareInfo();

    std::string GetRendererTag() const;
    std::string GenerateFilename() const;

    RenderEngine::Renderer* m_renderer = nullptr;
    bool m_recording = false;

    std::ofstream m_file;
    int m_frameIndex = 0;
    Timer m_sessionTimer;

    std::vector<BenchmarkMetric> m_customMetrics;

    // Per-session accumulators
    int m_minFPS = 0;
    int m_maxFPS = 0;
    double m_sumFPS = 0.0;
    float m_minFrameTime = 0.0f;
    float m_maxFrameTime = 0.0f;
    double m_sumFrameTime = 0.0;
    double m_sumTriangles = 0.0;
    double m_sumCPU = 0.0;
    double m_sumRAM = 0.0;

    // Stutter/spike counters
    int m_spikes16 = 0;   // frames > 16.67ms (below 60 FPS)
    int m_spikes33 = 0;   // frames > 33.33ms (below 30 FPS)
    int m_spikes50 = 0;   // frames > 50ms    (below 20 FPS)

    std::vector<float> m_frameTimes;   // all frame times for percentile calculation
    std::vector<double> m_customSums;
};

}  // namespace Sleak
