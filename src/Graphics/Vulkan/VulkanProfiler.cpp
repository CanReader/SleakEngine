#include <Debug/Benchmark.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "../../include/private/Graphics/Vulkan/VulkanRenderer.hpp"
#include "Core/Logger.hpp"

namespace Sleak {
namespace RenderEngine {

namespace {

constexpr float kGpuLogIntervalSeconds = 2.0f;

constexpr std::array<const char*, 12> kGpuRowNames = {
    "shadow", "gbuffer", "ssao",    "lighting", "forward", "taa",
    "ssr",    "bloom",   "tonemap", "ui",       "other",   "total"};

}  // namespace

/// Creates the timestamp query pool; leaves profiling off when the graphics
/// queue has no timestamp support.
bool VulkanRenderer::CreateGpuProfiler() {
    static_assert(kGpuRowNames.size() == GPU_ROW_COUNT);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physicalDevice, &props);

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount,
                                             nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount,
                                             families.data());

    const uint32_t validBits =
        QueueIDs.GraphicsIndex < familyCount
            ? families[QueueIDs.GraphicsIndex].timestampValidBits
            : 0;
    if (validBits == 0 || props.limits.timestampPeriod <= 0.0f) {
        SLEAK_WARN(
            "GPU profiler: graphics queue has no timestamp support, "
            "--gpuprofile ignored");
        return false;
    }

    VkQueryPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = MAX_FRAMES_IN_FLIGHT * GPU_QUERIES_PER_FRAME;
    if (vkCreateQueryPool(device, &info, nullptr, &m_gpuQueryPool) !=
        VK_SUCCESS) {
        SLEAK_WARN("GPU profiler: query pool creation failed");
        m_gpuQueryPool = VK_NULL_HANDLE;
        return false;
    }

    m_gpuTickNs = static_cast<double>(props.limits.timestampPeriod);
    m_gpuTickMask = validBits >= 64 ? ~0ull : ((1ull << validBits) - 1);
    m_gpuSlots = {};
    m_gpuSumMs = {};
    m_gpuMaxMs = {};
    m_gpuRuns = {};
    m_gpuLastMs = {};
    m_gpuWindowFrames = 0;
    m_gpuWindowTimer.Reset();
    m_gpuProfilerEnabled = true;

    SLEAK_INFO("GPU profiler: on ({} timestamp bits, {:.3f} ns/tick)",
               validBits, m_gpuTickNs);
    return true;
}

/// Destroys the timestamp query pool.
void VulkanRenderer::CleanupGpuProfiler() {
    m_gpuProfilerEnabled = false;
    if (m_gpuQueryPool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device, m_gpuQueryPool, nullptr);
        m_gpuQueryPool = VK_NULL_HANDLE;
    }
}

/// Folds this slot's finished timestamps into the running window and logs the
/// table once the window elapses. Call after the slot's fence.
void VulkanRenderer::CollectGpuProfile() {
    if (!m_gpuProfilerEnabled) return;
    GpuProfileSlot& slot = m_gpuSlots[currentFrame];
    if (!slot.submitted) return;
    slot.submitted = false;

    // value + availability per query
    const uint32_t queryCount = 2 + 2 * slot.pairCount;
    std::array<uint64_t, GPU_QUERIES_PER_FRAME * 2> data{};
    const VkResult res = vkGetQueryPoolResults(
        device, m_gpuQueryPool, currentFrame * GPU_QUERIES_PER_FRAME,
        queryCount, queryCount * 2 * sizeof(uint64_t), data.data(),
        2 * sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (res != VK_SUCCESS || data[1] == 0 || data[3] == 0) return;

    auto ticksToMs = [this](uint64_t begin, uint64_t end) {
        const uint64_t ticks = (end - begin) & m_gpuTickMask;
        return static_cast<float>(static_cast<double>(ticks) * m_gpuTickNs *
                                  1e-6);
    };

    std::array<float, GPU_ROW_COUNT> frameMs{};
    std::array<bool, GPU_ROW_COUNT> ran{};
    float passSum = 0.0f;
    for (uint32_t i = 0; i < slot.pairCount; ++i) {
        const uint64_t* begin = &data[(2 + 2 * i) * 2];
        const uint64_t* end = &data[(3 + 2 * i) * 2];
        if (begin[1] == 0 || end[1] == 0) continue;
        const float ms = ticksToMs(begin[0], end[0]);
        const auto row = static_cast<uint32_t>(slot.pairPass[i]);
        frameMs[row] += ms;
        ran[row] = true;
        passSum += ms;
    }
    frameMs[GPU_ROW_TOTAL] = ticksToMs(data[0], data[2]);
    frameMs[GPU_ROW_OTHER] = std::max(0.0f, frameMs[GPU_ROW_TOTAL] - passSum);
    ran[GPU_ROW_TOTAL] = true;
    ran[GPU_ROW_OTHER] = true;

    for (uint32_t row = 0; row < GPU_ROW_COUNT; ++row) {
        m_gpuLastMs[row] = ran[row] ? frameMs[row] : 0.0f;
        if (!ran[row]) continue;
        m_gpuSumMs[row] += frameMs[row];
        m_gpuMaxMs[row] = std::max(m_gpuMaxMs[row], frameMs[row]);
        ++m_gpuRuns[row];
    }
    ++m_gpuWindowFrames;

    const float elapsed = m_gpuWindowTimer.Elapsed();
    if (elapsed < kGpuLogIntervalSeconds) return;
    LogGpuProfile(elapsed);
    m_gpuSumMs = {};
    m_gpuMaxMs = {};
    m_gpuRuns = {};
    m_gpuWindowFrames = 0;
    m_gpuWindowTimer.Reset();
}

/// Resets this slot's queries and stamps the frame start.
void VulkanRenderer::BeginGpuFrame() {
    if (!m_gpuProfilerEnabled) return;
    GpuProfileSlot& slot = m_gpuSlots[currentFrame];
    slot = {};
    slot.recording = true;
    const uint32_t base = currentFrame * GPU_QUERIES_PER_FRAME;
    vkCmdResetQueryPool(command, m_gpuQueryPool, base, GPU_QUERIES_PER_FRAME);
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        m_gpuQueryPool, base);
}

/// Closes any pass left open and stamps the frame end.
void VulkanRenderer::EndGpuFrame() {
    if (!m_gpuProfilerEnabled) return;
    GpuProfileSlot& slot = m_gpuSlots[currentFrame];
    if (!slot.recording) return;
    if (slot.pairOpen) EndGpuPass(slot.pairPass[slot.pairCount]);
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        m_gpuQueryPool,
                        currentFrame * GPU_QUERIES_PER_FRAME + 1);
    slot.recording = false;
}

/// Flags this slot's queries for readback on its next fence wait.
void VulkanRenderer::MarkGpuFrameSubmitted() {
    if (!m_gpuProfilerEnabled) return;
    m_gpuSlots[currentFrame].submitted = true;
}

/// Stamps the start of a pass. Timestamps use BOTTOM_OF_PIPE so each pair
/// measures the time the pass adds once earlier work has drained.
void VulkanRenderer::BeginGpuPass(GpuPass pass) {
    if (!m_gpuProfilerEnabled) return;
    GpuProfileSlot& slot = m_gpuSlots[currentFrame];
    if (!slot.recording || slot.pairOpen || slot.pairCount >= GPU_MAX_PAIRS)
        return;
    vkCmdWriteTimestamp(
        command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_gpuQueryPool,
        currentFrame * GPU_QUERIES_PER_FRAME + 2 + 2 * slot.pairCount);
    slot.pairPass[slot.pairCount] = pass;
    slot.pairOpen = true;
}

/// Stamps the end of the pass opened by the matching BeginGpuPass.
void VulkanRenderer::EndGpuPass(GpuPass pass) {
    if (!m_gpuProfilerEnabled) return;
    GpuProfileSlot& slot = m_gpuSlots[currentFrame];
    if (!slot.recording || !slot.pairOpen ||
        slot.pairPass[slot.pairCount] != pass)
        return;
    vkCmdWriteTimestamp(
        command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_gpuQueryPool,
        currentFrame * GPU_QUERIES_PER_FRAME + 3 + 2 * slot.pairCount);
    slot.pairOpen = false;
    ++slot.pairCount;
}

/// Logs the averaged per-pass table for the current window.
void VulkanRenderer::LogGpuProfile(float seconds) const {
    SLEAK_INFO("GPU profile: {} frames in {:.1f} s, avg / max ms",
               m_gpuWindowFrames, seconds);
    for (uint32_t row = 0; row < GPU_ROW_COUNT; ++row) {
        if (m_gpuRuns[row] == 0) continue;
        const double avg = m_gpuSumMs[row] / m_gpuRuns[row];
        const std::string runs =
            m_gpuRuns[row] < m_gpuWindowFrames
                ? std::format("  ({}/{} frames)", m_gpuRuns[row],
                              m_gpuWindowFrames)
                : std::string();
        SLEAK_INFO("  {:<9}{:8.3f}{:8.3f}{}", kGpuRowNames[row], avg,
                   m_gpuMaxMs[row], runs);
    }
}

/// Adds per-pass GPU millisecond columns when --gpuprofile is on. Values lag
/// the CSV row by MAX_FRAMES_IN_FLIGHT frames.
void VulkanRenderer::RegisterBenchmarkMetrics(Benchmark& bench) {
    if (!m_gpuProfilerEnabled) return;
    for (uint32_t row = 0; row < GPU_ROW_COUNT; ++row) {
        if (row == GPU_ROW_OTHER) continue;
        bench.RegisterMetric(std::format("GPU_{}_ms", kGpuRowNames[row]),
                             [this, row] { return m_gpuLastMs[row]; });
    }
}

}  // namespace RenderEngine
}  // namespace Sleak
