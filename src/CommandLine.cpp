#include <Core/CommandLine.hpp>
#include <iostream>

namespace Sleak {

std::unordered_map<std::string, std::string> CommandLine::s_values;
std::unordered_set<std::string>              CommandLine::s_flags;
void(*CommandLine::s_helpCallback)(const char* exe) = nullptr;

void CommandLine::SetHelpCallback(void(*callback)(const char* exe)) {
    s_helpCallback = callback;
}

void CommandLine::Parse(int argc, char** argv) {
    s_values.clear();
    s_flags.clear();

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.empty()) continue;

        if (a == "help" || a == "--help" || a == "-help") {
            if (s_helpCallback) s_helpCallback(argv[0]);
            continue;
        }

        if (a.size() >= 2 && a[0] == '-' && a[1] == '-') {
            // Boolean flag: --bench, --fullscreen, --vsync …
            s_flags.insert(a);
        } else if (a[0] == '-') {
            // Key-value flag: -r vulkan   -w 1920   -world MyWorld …
            // Next token is the value if it doesn't start with '-'.
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                s_values[a] = argv[++i];
            } else {
                // No value → treat as boolean flag
                s_flags.insert(a);
            }
        }
    }
}

std::string CommandLine::GetValue(const std::string& flag,
                                   const std::string& defaultVal) {
    auto it = s_values.find(flag);
    return (it != s_values.end()) ? it->second : defaultVal;
}

bool CommandLine::HasFlag(const std::string& flag) {
    return s_flags.count(flag) > 0;
}

}  // namespace Sleak
