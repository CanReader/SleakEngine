#pragma once

#include <Core/OSDef.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Sleak {

// Pure command-line parser.
// Call Parse(argc, argv) once from main() before constructing Application.
// Any module can then read values via GetValue() / HasFlag().
//
// Parsing rules
// ─────────────
//  -flag value   → stored as key/value pair  (e.g. -r vulkan, -w 1920)
//  --flag        → stored as boolean flag     (e.g. --bench, --fullscreen)
//  --help / help → calls the registered help callback (if any), then continues
//
// Game-specific flags are NOT defined here.
// Use GetValue("-seed") / HasFlag("--fly") etc. from within the Game module.
class ENGINE_API CommandLine {
public:
    // Must be called once from main() before Application is constructed.
    static void Parse(int argc, char** argv);

    // Optionally set a help printer before calling Parse().
    // Called automatically when --help / help is encountered.
    // If not set, --help is silently ignored.
    static void SetHelpCallback(void(*callback)(const char* exe));

    // Returns the raw string value for -flag, or defaultVal if not present.
    static std::string GetValue(const std::string& flag,
                                const std::string& defaultVal = "");

    // Returns true if --flag was present.
    static bool HasFlag(const std::string& flag);

private:
    static void(*s_helpCallback)(const char* exe);
    static std::unordered_map<std::string, std::string> s_values;
    static std::unordered_set<std::string>              s_flags;
};

}  // namespace Sleak
