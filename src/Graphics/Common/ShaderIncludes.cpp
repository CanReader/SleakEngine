#include "../../include/private/Graphics/Common/ShaderIncludes.hpp"

#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

namespace Sleak {
namespace RenderEngine {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view WHITESPACE = " \t";
constexpr std::string_view VERSION_DIRECTIVE = "version";
constexpr std::string_view INCLUDE_DIRECTIVE = "include";
constexpr std::string_view EXTENSION_DIRECTIVE = "extension";
constexpr std::string_view INCLUDE_EXTENSION = "GL_GOOGLE_include_directive";
constexpr std::array<std::string_view, 2> LOG_PREFIXES = {"ERROR: ",
                                                          "WARNING: "};
constexpr size_t ROOT_FILE = 0;
constexpr size_t FIRST_LINE = 1;

/// Drops leading spaces and tabs.
std::string_view TrimLeft(std::string_view text) {
    const size_t start = text.find_first_not_of(WHITESPACE);
    return start == std::string_view::npos ? std::string_view{}
                                           : text.substr(start);
}

/// Splits a preprocessor line into its directive name and the rest.
std::optional<std::pair<std::string_view, std::string_view>> SplitDirective(
    std::string_view line) {
    line = TrimLeft(line);
    if (line.empty() || line.front() != '#') return std::nullopt;
    line = TrimLeft(line.substr(1));
    size_t end = 0;
    while (end < line.size() &&
           (std::isalnum(static_cast<unsigned char>(line[end])) ||
            line[end] == '_'))
        ++end;
    return std::pair{line.substr(0, end), TrimLeft(line.substr(end))};
}

/// True when a line is the given preprocessor directive.
bool IsDirective(std::string_view line, std::string_view name) {
    const auto directive = SplitDirective(line);
    return directive && directive->first == name;
}

/// True for the #extension line that turns #include on in glslc.
bool IsIncludeExtension(std::string_view line) {
    const auto directive = SplitDirective(line);
    return directive && directive->first == EXTENSION_DIRECTIVE &&
           directive->second.starts_with(INCLUDE_EXTENSION);
}

/// Reads a whole file; false when it cannot be opened.
bool ReadText(const fs::path& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    std::ostringstream text;
    text << file.rdbuf();
    out = text.str();
    return true;
}

/// Splits text into lines without their line breaks.
std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}

/// Appends a #line naming the next line's number and source string.
void AppendLineMarker(std::string& out, size_t line, size_t file) {
    out += std::format("#line {} {}\n", line, file);
}

/// Inlines lines [first, end) of one file into result, recursing into
/// includes. False with result.error set on the first failure.
bool AppendFile(const fs::path& file, const std::vector<std::string>& lines,
                size_t first, size_t fileIndex, int depth,
                const std::vector<fs::path>& roots, ExpandedShader& result) {
    for (size_t i = first; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        const size_t lineNumber = i + FIRST_LINE;
        if (IsIncludeExtension(line) || IsDirective(line, VERSION_DIRECTIVE)) {
            result.source += '\n';
            continue;
        }
        const auto include = ParseShaderInclude(line);
        if (!include) {
            result.source += line;
            result.source += '\n';
            continue;
        }
        const std::string where =
            std::format("{}:{}", file.generic_string(), lineNumber);
        if (depth >= MAX_SHADER_INCLUDE_DEPTH) {
            result.error = std::format("{}: includes nest deeper than {}",
                                       where, MAX_SHADER_INCLUDE_DEPTH);
            return false;
        }
        const auto child =
            ResolveShaderInclude(*include, file.parent_path(), roots);
        std::string text;
        if (!child || !ReadText(*child, text)) {
            result.error = std::format("{}: cannot open include \"{}\"", where,
                                       include->name);
            return false;
        }
        const size_t childIndex = result.files.size();
        result.files.push_back(child->generic_string());
        AppendLineMarker(result.source, FIRST_LINE, childIndex);
        if (!AppendFile(*child, SplitLines(text), 0, childIndex, depth + 1,
                        roots, result))
            return false;
        AppendLineMarker(result.source, lineNumber + 1, fileIndex);
    }
    return true;
}

}  // namespace

std::optional<ShaderIncludeDirective> ParseShaderInclude(
    std::string_view line) {
    const auto directive = SplitDirective(line);
    if (!directive || directive->first != INCLUDE_DIRECTIVE)
        return std::nullopt;
    const std::string_view rest = directive->second;
    if (rest.empty() || (rest.front() != '"' && rest.front() != '<'))
        return std::nullopt;
    const char close = rest.front() == '"' ? '"' : '>';
    const size_t end = rest.find(close, 1);
    if (end == std::string_view::npos) return std::nullopt;
    return ShaderIncludeDirective{std::string(rest.substr(1, end - 1)),
                                  close == '"'};
}

std::optional<fs::path> ResolveShaderInclude(
    const ShaderIncludeDirective& include, const fs::path& includerDir,
    const std::vector<fs::path>& roots) {
    std::error_code ec;
    if (include.quoted) {
        const fs::path local = includerDir / include.name;
        if (fs::is_regular_file(local, ec)) return local;
    }
    for (const fs::path& root : roots) {
        const fs::path candidate = root / include.name;
        if (fs::is_regular_file(candidate, ec)) return candidate;
    }
    return std::nullopt;
}

ExpandedShader ExpandShaderIncludes(const fs::path& file,
                                    const std::vector<fs::path>& roots) {
    ExpandedShader result;
    std::string text;
    if (!ReadText(file, text)) {
        result.error = std::format("cannot open {}", file.generic_string());
        return result;
    }
    const std::vector<std::string> lines = SplitLines(text);
    result.files.push_back(file.generic_string());
    size_t first = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (!IsDirective(lines[i], VERSION_DIRECTIVE)) continue;
        result.source = lines[i] + '\n';
        first = i + 1;
        AppendLineMarker(result.source, first + FIRST_LINE, ROOT_FILE);
        break;
    }
    if (!AppendFile(file, lines, first, ROOT_FILE, 0, roots, result))
        result.source.clear();
    return result;
}

std::string MapShaderLog(std::string_view log,
                         const std::vector<std::string>& files) {
    std::string mapped;
    size_t start = 0;
    while (start < log.size()) {
        size_t end = log.find('\n', start);
        if (end == std::string_view::npos) end = log.size();
        const std::string_view line = log.substr(start, end - start);
        size_t digits = 0;
        for (std::string_view prefix : LOG_PREFIXES) {
            if (line.starts_with(prefix)) {
                digits = prefix.size();
                break;
            }
        }
        size_t index = 0;
        const auto [next, ec] = std::from_chars(
            line.data() + digits, line.data() + line.size(), index);
        const size_t used = static_cast<size_t>(next - line.data());
        const bool numbered = ec == std::errc() && used < line.size() &&
                              (line[used] == ':' || line[used] == '(') &&
                              index < files.size();
        if (numbered) {
            mapped += line.substr(0, digits);
            mapped += files[index];
            mapped += line.substr(used);
        } else {
            mapped += line;
        }
        if (end < log.size()) mapped += '\n';
        start = end + 1;
    }
    return mapped;
}

bool ShaderDependsOn(const fs::path& shader, const fs::path& target,
                     const std::vector<fs::path>& roots) {
    std::error_code ec;
    const fs::path wanted = fs::weakly_canonical(target, ec);
    std::vector<std::pair<fs::path, int>> stack{{shader, 0}};
    std::set<fs::path> visited;
    while (!stack.empty()) {
        const auto [file, depth] = stack.back();
        stack.pop_back();
        if (depth > MAX_SHADER_INCLUDE_DEPTH ||
            !visited.insert(fs::weakly_canonical(file, ec)).second)
            continue;
        std::string text;
        if (!ReadText(file, text)) continue;
        for (const std::string& line : SplitLines(text)) {
            const auto include = ParseShaderInclude(line);
            if (!include) continue;
            const auto child =
                ResolveShaderInclude(*include, file.parent_path(), roots);
            if (!child) continue;
            if (fs::weakly_canonical(*child, ec) == wanted) return true;
            stack.emplace_back(*child, depth + 1);
        }
    }
    return false;
}

}  // namespace RenderEngine
}  // namespace Sleak
