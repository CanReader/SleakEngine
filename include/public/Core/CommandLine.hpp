#pragma once

#include <Core/OSDef.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Sleak {

/// Process-wide command line table. Parse once in main(), read anywhere.
///
/// Call Parse(argc, argv) from `main()` before constructing an
/// Application. The Application reads its window size, title, and graphics
/// backend out of this table, so skipping the call silently disables every
/// launch flag.
///
/// Parsing rules:
/// - `-flag value` is stored as a key/value pair, read with GetValue().
///   For example `-r vulkan` or `-w 1920`.
/// - `--flag` is stored as a boolean, read with HasFlag(). For example
///   `--fullscreen`.
/// - `--help` or `help` invokes the callback registered with
///   SetHelpCallback(), then parsing continues. Without a callback it is
///   ignored.
///
/// Engine-recognized flags are `-r` (backend: vulkan, opengl, d3d11,
/// d3d12), `-w` and `-h` (window size), `-t` (window title, with `_`
/// standing in for spaces), and `--fullscreen`. Game-specific flags are
/// not declared anywhere: just read whatever you like with
/// GetValue("-seed") or HasFlag("--fly") from your own code.
///
/// Everything is static and shared for the process.
///
/// @code{.cpp}
/// static void PrintHelp(const char* exe) {
///     std::printf("Usage: %s [-r backend] [-w n] [-h n] [-seed n]\n", exe);
/// }
///
/// int main(int argc, char** argv) {
///     Sleak::CommandLine::SetHelpCallback(PrintHelp);
///     Sleak::CommandLine::Parse(argc, argv);
///     Sleak::Logger::Init("MyGame");
///     // ... construct Application and Run
/// }
///
/// // Later, in game code
/// uint32_t seed = std::stoul(Sleak::CommandLine::GetValue("-seed", "0"));
/// bool flyMode  = Sleak::CommandLine::HasFlag("--fly");
/// @endcode
///
/// @see Application, ApplicationDefaults, Arguments, Logger
/// @ingroup core
class ENGINE_API CommandLine {
public:
    /// Parses argv into the value/flag tables. Call once from main(), before Application.
    static void Parse(int argc, char** argv);

    /// Registers the printer invoked for `--help`. Set it before Parse();
    /// without one, `--help` is ignored.
    static void SetHelpCallback(void(*callback)(const char* exe));

    /// Returns the raw string value stored for `-flag`, or defaultVal if it was not passed.
    static std::string GetValue(const std::string& flag,
                                const std::string& defaultVal = "");

    /// True if `--flag` was present on the command line.
    static bool HasFlag(const std::string& flag);

private:
    static void(*s_helpCallback)(const char* exe);
    static std::unordered_map<std::string, std::string> s_values;
    static std::unordered_set<std::string>              s_flags;
};

}  // namespace Sleak
