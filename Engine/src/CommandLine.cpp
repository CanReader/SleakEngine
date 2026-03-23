#include <Core/CommandLine.hpp>
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace Sleak {

// ── Static storage ────────────────────────────────────────────────────────────
std::unordered_map<std::string, std::string> CommandLine::s_values;
std::unordered_set<std::string>              CommandLine::s_flags;

// ── Parse ─────────────────────────────────────────────────────────────────────
void CommandLine::Parse(int argc, char** argv) {
    s_values.clear();
    s_flags.clear();

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.empty()) continue;

        if (a == "help" || a == "--help" || a == "-help") {
            PrintHelp(argv[0]);
            continue;
        }

        if (a.size() >= 2 && a[0] == '-' && a[1] == '-') {
            // Boolean flag: --bench, --benchmark, --help …
            s_flags.insert(a);
        } else if (a[0] == '-') {
            // Value flag: -r vulkan   -w 1920   -world MyWorld …
            // If the next token exists and does NOT start with '-', it is the value.
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                s_values[a] = argv[i + 1];
                ++i;
            } else {
                // Flag without value → treat as boolean
                s_flags.insert(a);
            }
        }
    }
}

// ── Typed getters ─────────────────────────────────────────────────────────────
int CommandLine::GetWidth() {
    auto it = s_values.find("-w");
    if (it != s_values.end() && !it->second.empty()) {
        try { return std::stoi(it->second); } catch (...) {}
    }
    return 1200;
}

int CommandLine::GetHeight() {
    auto it = s_values.find("-h");
    if (it != s_values.end() && !it->second.empty()) {
        try { return std::stoi(it->second); } catch (...) {}
    }
    return 800;
}

std::string CommandLine::GetTitle() {
    auto it = s_values.find("-t");
    if (it == s_values.end() || it->second.empty()) return "";
    std::string t = it->second;
    std::replace(t.begin(), t.end(), '_', ' ');
    return t;
}

std::string CommandLine::GetRenderer() {
    auto it = s_values.find("-r");
    return (it != s_values.end()) ? it->second : "";
}

std::string CommandLine::GetWorldName() {
    auto it = s_values.find("-world");
    return (it != s_values.end()) ? it->second : "";
}

bool CommandLine::AutoBenchmark() {
    return s_flags.count("--bench") || s_flags.count("--benchmark");
}

// ── Generic access ────────────────────────────────────────────────────────────
bool CommandLine::HasFlag(const std::string& flag) {
    return s_flags.count(flag) > 0;
}

std::string CommandLine::GetValue(const std::string& flag,
                                   const std::string& defaultVal) {
    auto it = s_values.find(flag);
    return (it != s_values.end()) ? it->second : defaultVal;
}

// ── Help ──────────────────────────────────────────────────────────────────────
void CommandLine::PrintHelp(const char* exe) {
    std::cout
        << "\nUsage: " << exe << " [OPTIONS]\n"
        << "\nRenderer\n"
        << "  -r vulkan          Use Vulkan\n"
        << "  -r d3d11           Use DirectX 11 (default on Windows)\n"
        << "  -r d3d12           Use DirectX 12\n"
        << "  -r opengl          Use OpenGL\n"
        << "\nWindow\n"
        << "  -w <pixels>        Window width   (default: 1200)\n"
        << "  -h <pixels>        Window height  (default: 800)\n"
        << "  -t <name>          Window title   (use _ for spaces)\n"
        << "\nWorld\n"
        << "  -world <name>      Load world if save exists, create new otherwise\n"
        << "\nBenchmark\n"
        << "  --bench            Start benchmark recording immediately on launch\n"
        << "\nMisc\n"
        << "  --help             Show this message\n"
        << "\nExamples\n"
        << "  SleakCraft -r vulkan -world MyWorld\n"
        << "  SleakCraft -r d3d11 -world TestWorld --bench\n"
        << "  SleakCraft -w 1920 -h 1080 -t My_Game\n\n";
}

}  // namespace Sleak
