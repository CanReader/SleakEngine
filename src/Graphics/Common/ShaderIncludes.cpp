#include "../../include/private/Graphics/Common/ShaderIncludes.hpp"

#include <algorithm>
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
constexpr std::string_view IFNDEF_DIRECTIVE = "ifndef";
constexpr std::string_view DEFINE_DIRECTIVE = "define";
constexpr std::string_view ENDIF_DIRECTIVE = "endif";
constexpr std::array<std::string_view, 3> CONDITIONAL_DIRECTIVES = {
    "if", "ifdef", "ifndef"};
constexpr std::string_view INCLUDE_EXTENSION = "GL_GOOGLE_include_directive";
constexpr std::string_view LINE_COMMENT = "//";
constexpr std::string_view BLOCK_COMMENT_OPEN = "/*";
constexpr std::string_view BLOCK_COMMENT_CLOSE = "*/";
constexpr std::string_view PARENT_DIR = "..";
constexpr std::array<std::string_view, 2> LOG_PREFIXES = {"ERROR: ",
                                                          "WARNING: "};
constexpr size_t ROOT_FILE = 0;
constexpr size_t FIRST_LINE = 1;
// #ifndef, #define and #endif
constexpr size_t GUARD_DIRECTIVES = 3;

/// Drops leading spaces and tabs.
std::string_view TrimLeft(std::string_view text) {
    const size_t start = text.find_first_not_of(WHITESPACE);
    return start == std::string_view::npos ? std::string_view{}
                                           : text.substr(start);
}

/// First space-separated word.
std::string_view FirstWord(std::string_view text) {
    text = TrimLeft(text);
    return text.substr(0, text.find_first_of(WHITESPACE));
}

/// A line with each comment turned into a space. inBlock carries an open
/// block comment in from the previous line and out to the next.
std::string StripComments(std::string_view line, bool& inBlock) {
    std::string code;
    size_t pos = 0;
    while (pos < line.size()) {
        if (inBlock) {
            const size_t close = line.find(BLOCK_COMMENT_CLOSE, pos);
            if (close == std::string_view::npos) break;
            inBlock = false;
            code += ' ';
            pos = close + BLOCK_COMMENT_CLOSE.size();
            continue;
        }
        const size_t lineComment = line.find(LINE_COMMENT, pos);
        const size_t blockComment = line.find(BLOCK_COMMENT_OPEN, pos);
        if (lineComment < blockComment) {
            code += line.substr(pos, lineComment - pos);
            break;
        }
        if (blockComment == std::string_view::npos) {
            code += line.substr(pos);
            break;
        }
        code += line.substr(pos, blockComment - pos);
        code += ' ';
        inBlock = true;
        pos = blockComment + BLOCK_COMMENT_OPEN.size();
    }
    return code;
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

/// True for #if, #ifdef and #ifndef.
bool OpensConditional(std::string_view directive) {
    return std::find(CONDITIONAL_DIRECTIVES.begin(),
                     CONDITIONAL_DIRECTIVES.end(),
                     directive) != CONDITIONAL_DIRECTIVES.end();
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

/// True when a file is one #ifndef NAME / #define NAME ... #endif block.
bool HasIncludeGuard(const std::vector<std::string>& lines) {
    std::vector<std::string> code;
    bool inBlock = false;
    for (const std::string& line : lines) {
        std::string stripped = StripComments(line, inBlock);
        if (!TrimLeft(stripped).empty()) code.push_back(std::move(stripped));
    }
    if (code.size() < GUARD_DIRECTIVES) return false;
    const auto open = SplitDirective(code[0]);
    const auto define = SplitDirective(code[1]);
    if (!open || open->first != IFNDEF_DIRECTIVE || !define ||
        define->first != DEFINE_DIRECTIVE)
        return false;
    const std::string_view guard = FirstWord(open->second);
    if (guard.empty() || FirstWord(define->second) != guard) return false;
    int depth = 0;
    for (size_t i = 0; i < code.size(); ++i) {
        const auto directive = SplitDirective(code[i]);
        if (!directive) continue;
        if (OpensConditional(directive->first)) {
            ++depth;
        } else if (directive->first == ENDIF_DIRECTIVE && --depth == 0) {
            return i + 1 == code.size();
        }
    }
    return false;
}

/// Why an include name is refused, or empty when it may be used.
std::string_view RefusedIncludeName(const std::string& name) {
    const fs::path path(name);
    if (path.empty()) return "is empty";
    if (path.has_root_name() || path.has_root_directory())
        return "is an absolute path";
    for (const fs::path& part : path) {
        if (part == fs::path(PARENT_DIR))
            return "uses .., which leaves the include folders";
    }
    return {};
}

/// One name for a file however it was reached.
fs::path FileKey(const fs::path& path) {
    std::error_code ec;
    fs::path key = fs::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : key;
}

/// Appends a #line naming the next line's number and source string.
void AppendLineMarker(std::string& out, size_t line, size_t file) {
    out += std::format("#line {} {}\n", line, file);
}

/// Stand-in for a dropped line that keeps the comment state of the output.
void AppendCommentBridge(std::string& out, bool openBefore, bool openAfter) {
    if (openBefore) out += BLOCK_COMMENT_CLOSE;
    if (openBefore && openAfter) out += ' ';
    if (openAfter) out += BLOCK_COMMENT_OPEN;
    out += '\n';
}

/// State shared by every file of one expansion.
struct Expansion {
    const std::vector<fs::path>& roots;
    ExpandedShader& result;
    // Files already inlined, and the ones open now, outermost first
    std::set<fs::path> seen;
    std::vector<fs::path> chain;
};

/// "a.glsl -> b.glsl -> a.glsl" for the open files from file onward.
std::string CycleText(const std::vector<fs::path>& chain,
                      const fs::path& file) {
    std::string text;
    for (auto it = std::find(chain.begin(), chain.end(), file);
         it != chain.end(); ++it)
        text += it->filename().generic_string() + " -> ";
    return text + file.filename().generic_string();
}

/// Inlines lines [first, end) of one file, recursing into includes. inBlock
/// is the comment state at first and, on return, at the end. False with
/// result.error set on the first failure.
bool AppendFile(Expansion& ctx, const fs::path& file,
                const std::vector<std::string>& lines, size_t first,
                size_t fileIndex, bool& inBlock) {
    std::string& out = ctx.result.source;
    for (size_t i = first; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        const size_t lineNumber = i + FIRST_LINE;
        const bool openBefore = inBlock;
        const std::string code = StripComments(line, inBlock);
        if (IsIncludeExtension(code) || IsDirective(code, VERSION_DIRECTIVE)) {
            AppendCommentBridge(out, openBefore, inBlock);
            continue;
        }
        const auto include = ParseShaderInclude(code);
        if (!include) {
            out += line;
            out += '\n';
            continue;
        }
        const std::string where =
            std::format("{}:{}", file.generic_string(), lineNumber);
        const std::string_view refused = RefusedIncludeName(include->name);
        if (!refused.empty()) {
            ctx.result.error = std::format("{}: include \"{}\" {}", where,
                                           include->name, refused);
            return false;
        }
        const auto child =
            ResolveShaderInclude(*include, file.parent_path(), ctx.roots);
        std::string text;
        if (!child || !ReadText(*child, text)) {
            ctx.result.error = std::format("{}: cannot open include \"{}\"",
                                           where, include->name);
            return false;
        }
        const fs::path key = FileKey(*child);
        const std::vector<std::string> childLines = SplitLines(text);
        const bool open = std::find(ctx.chain.begin(), ctx.chain.end(), key) !=
                          ctx.chain.end();
        if (open && !HasIncludeGuard(childLines)) {
            ctx.result.error = std::format("{}: include cycle {}", where,
                                           CycleText(ctx.chain, key));
            return false;
        }
        if (ctx.seen.contains(key)) {
            AppendCommentBridge(out, openBefore, inBlock);
            continue;
        }
        if (ctx.chain.size() > static_cast<size_t>(MAX_SHADER_INCLUDE_DEPTH)) {
            ctx.result.error = std::format("{}: includes nest deeper than {}",
                                           where, MAX_SHADER_INCLUDE_DEPTH);
            return false;
        }
        ctx.seen.insert(key);
        if (openBefore) AppendCommentBridge(out, true, false);
        const size_t childIndex = ctx.result.files.size();
        ctx.result.files.push_back(child->generic_string());
        AppendLineMarker(out, FIRST_LINE, childIndex);
        ctx.chain.push_back(key);
        bool childBlock = false;
        if (!AppendFile(ctx, *child, childLines, 0, childIndex, childBlock))
            return false;
        ctx.chain.pop_back();
        if (childBlock) AppendCommentBridge(out, true, false);
        if (inBlock) {
            AppendLineMarker(out, lineNumber, fileIndex);
            AppendCommentBridge(out, false, true);
        } else {
            AppendLineMarker(out, lineNumber + 1, fileIndex);
        }
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
    if (!RefusedIncludeName(include.name).empty()) return std::nullopt;
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
    Expansion ctx{roots, result, {}, {}};
    ctx.seen.insert(FileKey(file));
    ctx.chain.push_back(FileKey(file));

    size_t first = 0;
    bool inBlock = false;
    bool scanBlock = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string code = StripComments(lines[i], scanBlock);
        if (!IsDirective(code, VERSION_DIRECTIVE)) continue;
        result.source = std::string(TrimLeft(code)) + '\n';
        first = i + 1;
        inBlock = scanBlock;
        if (inBlock) {
            AppendLineMarker(result.source, first, ROOT_FILE);
            AppendCommentBridge(result.source, false, true);
        } else {
            AppendLineMarker(result.source, first + FIRST_LINE, ROOT_FILE);
        }
        break;
    }
    if (!AppendFile(ctx, file, lines, first, ROOT_FILE, inBlock))
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
    const fs::path wanted = FileKey(target);
    std::vector<std::pair<fs::path, int>> stack{{shader, 0}};
    std::set<fs::path> visited;
    while (!stack.empty()) {
        const auto [file, depth] = stack.back();
        stack.pop_back();
        if (depth > MAX_SHADER_INCLUDE_DEPTH ||
            !visited.insert(FileKey(file)).second)
            continue;
        std::string text;
        if (!ReadText(file, text)) continue;
        bool inBlock = false;
        for (const std::string& line : SplitLines(text)) {
            const auto include =
                ParseShaderInclude(StripComments(line, inBlock));
            if (!include) continue;
            const auto child =
                ResolveShaderInclude(*include, file.parent_path(), roots);
            if (!child) continue;
            if (FileKey(*child) == wanted) return true;
            stack.emplace_back(*child, depth + 1);
        }
    }
    return false;
}

}  // namespace RenderEngine
}  // namespace Sleak
