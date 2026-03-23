#pragma once

#include <Core/OSDef.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Sleak {

// Static command-line parser.
// Call CommandLine::Parse(argc, argv) once from main() before constructing
// Application.  After that any module (Engine or Game) can call the typed
// getters without carrying argc/argv through constructors.
//
// Supported flags
// ───────────────
//  -r  <api>        Renderer: vulkan | d3d11 | d3d12 | opengl
//  -w  <n>          Window width          (default 1200)
//  -h  <n>          Window height         (default  800)
//  -t  <name>       Window title; use _ for spaces
//  -world <name>    World to open (load if save exists, else create with random seed)
//  -seed <n>        Seed for new world creation (default: random)
//  -rd <n>          Initial render distance in chunks (default: 8)
//  -msaa <n>        MSAA sample count: 1 | 2 | 4 | 8  (default: 1)
//  --vsync          Enable VSync on launch
//  --no-vsync       Disable VSync on launch
//  --fullscreen     Start in fullscreen
//  --fly            Start in fly mode
//  --bench / --benchmark   Auto-start benchmark recording on launch
//  --help / help           Print help and continue
class ENGINE_API CommandLine {
public:
    // Must be called once from main() before Application is constructed.
    static void Parse(int argc, char** argv);

    // Typed accessors
    static int         GetWidth();          // -w  (default 1200)
    static int         GetHeight();         // -h  (default 800)
    static std::string GetTitle();          // -t  (underscores → spaces)
    static std::string GetRenderer();       // -r
    static std::string GetWorldName();      // -world
    static int         GetSeed();           // -seed  (0 = random)
    static int         GetRenderDistance(); // -rd    (0 = use default)
    static int         GetMSAA();           // -msaa  (0 = use default)
    static int         GetVSync();          // 1=on, -1=off, 0=unset
    static bool        StartFullscreen();   // --fullscreen
    static bool        StartFlyMode();      // --fly
    static bool        AutoBenchmark();     // --bench / --benchmark

    // Generic access for custom flags
    static bool        HasFlag (const std::string& flag);
    static std::string GetValue(const std::string& flag,
                                const std::string& defaultVal = "");

private:
    static void PrintHelp(const char* exe);

    // -flag value pairs
    static std::unordered_map<std::string, std::string> s_values;
    // Boolean flags (--bench, --help, …)
    static std::unordered_set<std::string>              s_flags;
};

}  // namespace Sleak
