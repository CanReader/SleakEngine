#include <Debug/ShaderHotReload.hpp>

#ifdef SLEAK_HOT_RELOAD
#include <Core/Application.hpp>
#include <Core/FileWatcher.hpp>
#include <Core/Logger.hpp>
#include <Core/Timer.hpp>
#include <Graphics/Common/Renderer.hpp>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>

#ifdef PLATFORM_WIN
#include <process.h>
#else
#include <unistd.h>
#endif
#endif

namespace Sleak {
namespace Debug {

#ifdef SLEAK_HOT_RELOAD
namespace {

namespace fs = std::filesystem;

constexpr float DEBOUNCE_SECONDS = 0.15f;
constexpr float MS_PER_SECOND = 1000.0f;
constexpr size_t PROCESS_LINE_SIZE = 512;
constexpr const char* RUNTIME_SHADER_DIR = "assets/shaders";
constexpr const char* SPIRV_EXTENSION = ".spv";
constexpr const char* TEMP_EXTENSION = ".tmp";
constexpr std::string_view GL_SUFFIX = "_gl";
constexpr std::string_view DX12_SUFFIX = "_dx12";

struct State {
    FileWatcher watcher;
    std::vector<fs::path> directories;
    std::set<fs::path> pending;
    Timer quietTimer;
};

/// Lazily created hot reload state shared by all calls.
State& GetState() {
    static State state;
    return state;
}

/// Per-process temp name next to a target, so two running games never clash.
fs::path TempPathFor(const fs::path& target) {
#ifdef PLATFORM_WIN
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    fs::path tmp = target;
    tmp += "." + std::to_string(pid) + TEMP_EXTENSION;
    return tmp;
}

/// Atomically replaces a target with a finished temp file.
bool ReplaceWith(const fs::path& tmp, const fs::path& target) {
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (!ec) return true;
    SLEAK_ERROR("Hot reload: can't replace {}: {}", target.filename().string(),
                ec.message());
    fs::remove(tmp, ec);
    return false;
}

/// True for shader sources the watcher reacts to.
bool IsShaderSource(const fs::path& file) {
    const fs::path ext = file.extension();
    return ext == ".vert" || ext == ".frag" || ext == ".hlsl";
}

/// True for GLSL that glslc compiles to SPIR-V for Vulkan.
bool IsVulkanSource(const fs::path& file) {
    const fs::path ext = file.extension();
    if (ext != ".vert" && ext != ".frag") return false;
    return !file.stem().string().ends_with(GL_SUFFIX);
}

/// Stem shared by all backend variants, e.g. lighting_pass_gl.frag.
std::string StemOf(const fs::path& file) {
    std::string name = file.filename().string();
    name = name.substr(0, name.find('.'));
    for (std::string_view suffix : {GL_SUFFIX, DX12_SUFFIX}) {
        if (name.ends_with(suffix)) {
            name.resize(name.size() - suffix.size());
            break;
        }
    }
    return name;
}

/// Backend that loads this source file, picked from its suffix.
RenderEngine::RendererType BackendOf(const fs::path& file) {
    using RenderEngine::RendererType;
    const std::string stem = file.stem().string();
    if (file.extension() == ".hlsl")
        return stem.ends_with(DX12_SUFFIX) ? RendererType::DirectX12
                                           : RendererType::DirectX11;
    return stem.ends_with(GL_SUFFIX) ? RendererType::OpenGL
                                     : RendererType::Vulkan;
}

/// File name pattern a backend loads for a stem.
std::string VariantFor(const std::string& stem,
                       RenderEngine::RendererType type) {
    using RenderEngine::RendererType;
    switch (type) {
        case RendererType::Vulkan:
            return stem + ".vert / " + stem + ".frag";
        case RendererType::OpenGL:
            return stem + "_gl.vert / " + stem + "_gl.frag";
        case RendererType::DirectX11:
            return stem + ".hlsl";
        case RendererType::DirectX12:
            return stem + "_dx12.hlsl";
    }
    return stem;
}

/// True when a later source folder has a file with the same name.
bool IsOverridden(const fs::path& file, const State& state) {
    const auto dir = std::find(state.directories.begin(),
                               state.directories.end(), file.parent_path());
    if (dir == state.directories.end()) return false;
    for (auto later = dir + 1; later != state.directories.end(); ++later) {
        std::error_code ec;
        if (fs::exists(*later / file.filename(), ec)) return true;
    }
    return false;
}

/// Compiles GLSL to SPIR-V next to the source. Keeps the old .spv on error.
bool CompileSpirv(const fs::path& src, const fs::path& spv) {
#ifndef SLEAK_GLSLC_PATH
    (void)spv;
    SLEAK_WARN("Hot reload: glslc was not found at configure time, {} skipped",
               src.filename().string());
    return false;
#else
    const fs::path tmp = TempPathFor(spv);
    std::string cmd = "\"" SLEAK_GLSLC_PATH "\" \"" + src.string() +
                      "\" -o \"" + tmp.string() + "\" 2>&1";
#ifdef PLATFORM_WIN
    cmd = "\"" + cmd + "\"";
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) {
        SLEAK_ERROR("Hot reload: can't run glslc for {}",
                    src.filename().string());
        return false;
    }
    std::string output;
    char line[PROCESS_LINE_SIZE];
    while (std::fgets(line, sizeof(line), pipe)) output += line;
#ifdef PLATFORM_WIN
    const int status = _pclose(pipe);
#else
    const int status = pclose(pipe);
#endif
    if (status != 0) {
        std::error_code ec;
        fs::remove(tmp, ec);
        SLEAK_ERROR(
            "Hot reload: {} failed to compile, keeping the old "
            "shader\n{}",
            src.filename().string(), output);
        return false;
    }
    return ReplaceWith(tmp, spv);
#endif
}

/// Copies a file into the runtime shader folder the backends load from.
bool CopyToRuntime(const fs::path& file) {
    const fs::path target = fs::path(RUNTIME_SHADER_DIR) / file.filename();
    const fs::path tmp = TempPathFor(target);
    std::error_code ec;
    fs::copy_file(file, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        SLEAK_ERROR("Hot reload: can't copy {}: {}", file.filename().string(),
                    ec.message());
        return false;
    }
    return ReplaceWith(tmp, target);
}

/// Compiles and stages one saved file. Returns its stem, or empty to skip.
std::string ProcessFile(const fs::path& file, const State& state) {
    std::error_code ec;
    if (!fs::exists(file, ec)) return {};
    if (IsOverridden(file, state)) {
        SLEAK_WARN(
            "Hot reload: {} is overridden by a later shader folder, "
            "edit that copy instead",
            file.filename().string());
        return {};
    }
    if (IsVulkanSource(file)) {
        fs::path spv = file;
        spv += SPIRV_EXTENSION;
        if (!CompileSpirv(file, spv) || !CopyToRuntime(spv)) return {};
    }
    if (!CopyToRuntime(file)) return {};
    return StemOf(file);
}

}  // namespace
#endif

void ShaderHotReload::AddSourceDirectory(const std::string& dir) {
#ifdef SLEAK_HOT_RELOAD
    std::error_code ec;
    const fs::path path = fs::canonical(dir, ec);
    if (ec) {
        SLEAK_WARN("Hot reload: shader folder {} not found", dir);
        return;
    }
    State& state = GetState();
    if (std::find(state.directories.begin(), state.directories.end(), path) !=
        state.directories.end())
        return;
    if (!state.watcher.AddDirectory(path.string())) return;
    state.directories.push_back(path);
    SLEAK_INFO("Hot reload: watching {}", path.string());
#else
    (void)dir;
#endif
}

void ShaderHotReload::Poll() {
#ifdef SLEAK_HOT_RELOAD
    State& state = GetState();
    if (state.directories.empty()) return;

    std::vector<std::string> changed;
    state.watcher.Poll(changed);
    for (const std::string& path : changed) {
        const fs::path file(path);
        if (!IsShaderSource(file)) continue;
        state.pending.insert(file);
        state.quietTimer.Reset();
    }
    if (state.pending.empty() || state.quietTimer.Elapsed() < DEBOUNCE_SECONDS)
        return;

    Application* app = Application::GetInstance();
    RenderEngine::Renderer* renderer = app ? app->GetRenderer() : nullptr;
    if (!renderer) return;
    const RenderEngine::RendererType active = renderer->GetType();

    Timer reloadTimer;
    std::vector<std::string> stems;
    for (const fs::path& file : state.pending) {
        std::string stem = ProcessFile(file, state);
        if (stem.empty()) continue;
        if (BackendOf(file) != active) {
            SLEAK_WARN(
                "Hot reload: {} is not used by the {} renderer, edit {} "
                "instead",
                file.filename().string(), app->GetRendererTypeStr(),
                VariantFor(stem, active));
            continue;
        }
        if (std::find(stems.begin(), stems.end(), stem) == stems.end())
            stems.push_back(std::move(stem));
    }
    state.pending.clear();
    if (stems.empty()) return;

    app->WaitGPUIdle();
    if (!renderer->ReloadShaders(stems)) {
        SLEAK_WARN("Hot reload: the {} backend can't reload shaders yet",
                   app->GetRendererTypeStr());
        return;
    }

    std::string names;
    for (const std::string& stem : stems)
        names += (names.empty() ? "" : ", ") + stem;
    SLEAK_INFO("Hot reload: reloaded {} in {:.0f} ms", names,
               reloadTimer.Elapsed() * MS_PER_SECOND);
#endif
}

}  // namespace Debug
}  // namespace Sleak
