#include "mc/diagnostics.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string_view>

namespace cl::mc {

namespace {

std::string lower_ascii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

std::string trim(std::string text) {
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

std::string clip(std::string text, size_t limit = 220) {
    text = trim(std::move(text));
    if (text.size() > limit) {
        text.resize(limit);
        text += "...";
    }
    return text;
}

std::optional<size_t> first_index(const std::vector<std::string>& lower,
                                  std::string_view needle) {
    for (size_t i = 0; i < lower.size(); ++i) {
        if (lower[i].find(needle) != std::string::npos) return i;
    }
    return std::nullopt;
}

std::optional<size_t> first_index_any(
    const std::vector<std::string>& lower,
    std::initializer_list<std::string_view> needles) {
    for (size_t i = 0; i < lower.size(); ++i) {
        for (const auto needle : needles) {
            if (lower[i].find(needle) != std::string::npos) return i;
        }
    }
    return std::nullopt;
}

std::string exit_hint(int code) {
    switch (code) {
        case 1:
            return "退出码 1，通常是模组、配置或启动参数错误。";
        case -1:
            return "退出码 -1（4294967295），通常是模组主动结束进程或加载器拒绝启动。";
        case -1073741819:
            return "退出码 0xC0000005，内存访问冲突，常见于显卡驱动或模组崩溃。";
        case -1073740791:
            return "退出码 0xC0000409，栈溢出或驱动崩溃。";
        case -1073741510:
            return "退出码 0xC000013A，进程被终止（启动器停止或 Ctrl+C）。";
        default:
            return "退出码 " + std::to_string(code) + "。";
    }
}

// 从 [12:34:56]、[2026-10-21 12:34:56] 或裸 HH:MM:SS 中取时间。
std::string timestamp_of(const std::string& line) {
    for (size_t open = line.find('['); open != std::string::npos;
         open = line.find('[', open + 1)) {
        const size_t close = line.find(']', open + 1);
        if (close == std::string::npos) break;
        const std::string inner = line.substr(open + 1, close - open - 1);
        if (inner.find(':') != std::string::npos && inner.size() <= 32) {
            return trim(inner);
        }
    }
    for (size_t i = 0; i + 8 <= line.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(line[i])) &&
            std::isdigit(static_cast<unsigned char>(line[i + 1])) &&
            line[i + 2] == ':' &&
            std::isdigit(static_cast<unsigned char>(line[i + 3])) &&
            std::isdigit(static_cast<unsigned char>(line[i + 4])) &&
            line[i + 5] == ':' &&
            std::isdigit(static_cast<unsigned char>(line[i + 6])) &&
            std::isdigit(static_cast<unsigned char>(line[i + 7]))) {
            return line.substr(i, 8);
        }
    }
    return {};
}

std::string timestamp_near(const std::vector<std::string>& lines,
                           size_t index) {
    if (index < lines.size()) {
        if (auto time = timestamp_of(lines[index]); !time.empty()) return time;
    }
    for (size_t back = 1; back <= 20 && index >= back; ++back) {
        if (auto time = timestamp_of(lines[index - back]); !time.empty()) {
            return time;
        }
    }
    return {};
}

std::string quoted_after(const std::string& line, std::string_view marker) {
    const std::string lower = lower_ascii(line);
    const std::string key = lower_ascii(std::string(marker));
    size_t pos = 0;
    if (!key.empty()) {
        pos = lower.find(key);
        if (pos == std::string::npos) return {};
        pos += key.size();
    }
    const size_t first = line.find('\'', pos);
    if (first == std::string::npos) return {};
    const size_t second = line.find('\'', first + 1);
    if (second == std::string::npos) return {};
    return clip(line.substr(first + 1, second - first - 1), 90);
}

std::vector<std::string> quoted_strings(const std::string& line) {
    std::vector<std::string> result;
    size_t pos = 0;
    while (true) {
        const size_t open = line.find('\'', pos);
        if (open == std::string::npos) break;
        const size_t close = line.find('\'', open + 1);
        if (close == std::string::npos) break;
        result.push_back(clip(line.substr(open + 1, close - open - 1), 90));
        pos = close + 1;
    }
    return result;
}

std::string token_after(const std::string& line, std::string_view marker) {
    const std::string lower = lower_ascii(line);
    const std::string key = lower_ascii(std::string(marker));
    const size_t pos = lower.find(key);
    if (pos == std::string::npos) return {};
    size_t begin = pos + key.size();
    while (begin < line.size() &&
           (line[begin] == ' ' || line[begin] == ':' || line[begin] == '\'' ||
            line[begin] == '"' || line[begin] == '(')) {
        ++begin;
    }
    size_t end = begin;
    while (end < line.size() &&
           !std::isspace(static_cast<unsigned char>(line[end])) &&
           line[end] != ',' && line[end] != ')' && line[end] != ']' &&
           line[end] != '\'' && line[end] != '"') {
        ++end;
    }
    return clip(line.substr(begin, end - begin), 90);
}

std::string evidence_near(const std::vector<std::string>& lines, size_t index,
                          size_t before = 2, size_t after = 5) {
    const size_t begin = index > before ? index - before : 0;
    const size_t end = std::min(lines.size(), index + after + 1);
    std::string text;
    for (size_t i = begin; i < end; ++i) {
        if (!text.empty()) text += '\n';
        text += lines[i];
        if (text.size() > 1800) {
            text.resize(1800);
            text += "\n...";
            break;
        }
    }
    return text;
}

void add_finding(std::vector<AnalysisFinding>& findings,
                 std::set<std::string>& seen, AnalysisFinding finding) {
    if (finding.id.empty() || finding.title.empty()) return;
    const std::string key =
        finding.id + "|" + finding.mod_a + "|" + finding.mod_b;
    if (!seen.insert(key).second) return;
    if (finding.severity.empty()) finding.severity = "error";
    findings.push_back(std::move(finding));
}

AnalysisFinding make_finding(std::string id, std::string category,
                             std::string title, std::string detail,
                             std::string severity) {
    AnalysisFinding finding;
    finding.id = std::move(id);
    finding.category = std::move(category);
    finding.title = std::move(title);
    finding.detail = std::move(detail);
    finding.severity = std::move(severity);
    return finding;
}

std::string java_major_for_class_version(int class_version) {
    if (class_version <= 0) return {};
    const int major = class_version - 44;
    if (major < 1 || major > 40) return {};
    return std::to_string(major);
}

int class_file_version(const std::string& lower) {
    constexpr std::string_view marker = "class file version";
    const size_t pos = lower.find(marker);
    if (pos == std::string::npos) return 0;
    size_t begin = pos + marker.size();
    while (begin < lower.size() &&
           !std::isdigit(static_cast<unsigned char>(lower[begin]))) {
        ++begin;
    }
    int value = 0;
    while (begin < lower.size() &&
           std::isdigit(static_cast<unsigned char>(lower[begin]))) {
        value = value * 10 + (lower[begin] - '0');
        ++begin;
    }
    return value;
}

RunSummary normal_summary(bool offline_auth) {
    RunSummary summary;
    summary.normal = true;
    summary.severity = "info";
    summary.title = "游戏已正常退出";
    if (offline_auth) {
        summary.detail =
            "本次运行没有发现错误。日志里的 401 和 Realms 报错来自离线模式："
            "没有正版登录令牌，单人游戏不受影响。";
        summary.suggestions = {
            "如果想显示正版皮肤或使用 Realms，需要登录 Microsoft 账号"};
    } else {
        summary.detail = "本次运行没有发现错误。";
    }
    return summary;
}

} // namespace

RunSummary analyze_run(int exit_code, const std::vector<std::string>& lines) {
    return analyze_run(exit_code, lines, AnalysisContext{});
}

RunSummary analyze_run(int exit_code, const std::vector<std::string>& lines,
                       const AnalysisContext& context) {
    std::vector<std::string> lower;
    lower.reserve(lines.size());
    for (const auto& line : lines) lower.push_back(lower_ascii(line));

    std::vector<AnalysisFinding> findings;
    std::set<std::string> seen;

    const bool offline_auth =
        first_index(lower, "failed to fetch user properties").has_value() ||
        first_index(lower, "could not authorize you against realms").has_value();

    // ---- 内存 ----
    for (size_t i = 0; i < lower.size(); ++i) {
        if (lower[i].find("outofmemoryerror") == std::string::npos) continue;
        const std::string& line = lines[i];
        if (lower[i].find("metaspace") != std::string::npos) {
            auto finding = make_finding(
                "memory.metaspace", "memory", "元空间不足",
                "Java 的元空间（Metaspace）耗尽了，通常是模组太多或 Java 版本过旧。",
                "error");
            finding.suggestions = {
                "换用较新的 64 位 Java（1.17+ 推荐 17，1.20.5+ 推荐 21）",
                "减少同时安装的模组数量",
                "把最大内存适当调高后重试"};
            finding.time = timestamp_near(lines, i);
            finding.event = clip(line, 600);
            finding.evidence = evidence_near(lines, i);
            add_finding(findings, seen, std::move(finding));
        } else if (lower[i].find("direct buffer memory") != std::string::npos) {
            auto finding = make_finding(
                "memory.direct", "memory", "直接内存不足",
                "Java 的直接内存（Direct Buffer）耗尽了，常见于光影、高清材质或渲染模组。",
                "error");
            finding.suggestions = {
                "降低渲染距离和材质分辨率", "关闭光影或减少渲染类模组",
                "适当提高最大内存并确认使用 64 位 Java"};
            finding.time = timestamp_near(lines, i);
            finding.event = clip(line, 600);
            finding.evidence = evidence_near(lines, i);
            add_finding(findings, seen, std::move(finding));
        } else if (lower[i].find("gc overhead") != std::string::npos) {
            auto finding = make_finding(
                "memory.gc", "memory", "内存回收压力过大",
                "Java 几乎全部时间都在回收内存，通常是内存太小或模组占用过高。",
                "error");
            finding.suggestions = {
                "把最大内存提高到 4096 MB 或更高（不要超过物理内存的一半）",
                "减少模组或降低渲染距离"};
            finding.time = timestamp_near(lines, i);
            finding.event = clip(line, 600);
            finding.evidence = evidence_near(lines, i);
            add_finding(findings, seen, std::move(finding));
        } else {
            std::string detail = "游戏运行时耗尽了内存。";
            if (context.max_memory_mb > 0) {
                detail += "本次设置的最大内存为 " +
                          std::to_string(context.max_memory_mb) + " MB。";
            }
            detail += " " + clip(line, 300);
            auto finding = make_finding("memory.heap", "memory", "内存不足",
                                        detail, "error");
            finding.suggestions = {
                "把最大内存调到 4096 MB 或更高（不要超过物理内存的一半）",
                "确认使用的是 64 位 Java",
                "降低渲染距离、减少模组或关闭光影"};
            finding.time = timestamp_near(lines, i);
            finding.event = clip(line, 600);
            finding.evidence = evidence_near(lines, i);
            add_finding(findings, seen, std::move(finding));
        }
        break;
    }
    if (const auto index = first_index_any(
            lower, {"could not reserve enough space",
                    "invalid maximum heap size",
                    "incompatible minimum and maximum heap sizes"})) {
        auto finding = make_finding(
            "memory.args", "memory", "内存参数不正确",
            "Java 无法分配你设置的内存值。", "error");
        finding.suggestions = {
            "把最大内存降到 2048–4096 MB",
            "确认 Java 是 64 位（32 位 Java 最多只能分配约 1.5 GB）",
            "最小内存不要大于最大内存"};
        finding.time = timestamp_near(lines, *index);
        finding.event = clip(lines[*index], 600);
        finding.evidence = evidence_near(lines, *index);
        add_finding(findings, seen, std::move(finding));
    }

    // ---- Java 版本 ----
    if (const auto index = first_index_any(
            lower, {"unsupportedclassversionerror",
                    "unsupported class file major version",
                    "has been compiled by a more recent version of the java "
                    "runtime"})) {
        const int version = class_file_version(lower[*index]);
        const std::string required = java_major_for_class_version(version);
        std::string detail = "当前 Java 版本低于游戏或模组要求。";
        if (!required.empty()) {
            detail += "日志显示类文件版本为 " + std::to_string(version) +
                      "，至少需要 Java " + required + "。";
        }
        if (context.java_major > 0) {
            detail += "本次启动使用的是 Java " +
                      std::to_string(context.java_major) + "。";
        }
        auto finding = make_finding("java.class_version", "java",
                                    "Java 版本不匹配", detail, "error");
        finding.suggestions = {
            required.empty()
                ? "1.17+ 使用 Java 17，1.20.5+ 使用 Java 21"
                : "安装并使用 Java " + required,
            "在启动器设置里手动指定对应版本的 java.exe",
            "不要用 Java 8 启动 1.17 及以上的版本"};
        finding.time = timestamp_near(lines, *index);
        finding.event = clip(lines[*index], 600);
        finding.evidence = evidence_near(lines, *index);
        add_finding(findings, seen, std::move(finding));
    }

    // ---- 启动器参数 ----
    if (const auto index =
            first_index(lower, "was already on the jvms module path")) {
        auto finding = make_finding(
            "launcher.module_path", "launcher", "启动器参数冲突",
            "classpath 与 module path 指向了同一个库，这是启动器拼参数的问题。",
            "error");
        finding.suggestions = {"更新启动器到最新版本",
                               "如果仍然出现，把这段日志发给启动器开发者"};
        finding.time = timestamp_near(lines, *index);
        finding.event = clip(lines[*index], 600);
        finding.evidence = evidence_near(lines, *index);
        add_finding(findings, seen, std::move(finding));
    }

    // ---- 缺少前置 / 依赖 ----
    bool dependency_found = false;
    for (size_t i = 0; i < lower.size(); ++i) {
        const std::string& line = lines[i];
        if (lower[i].find("mod id:") != std::string::npos &&
            (lower[i].find("requested by") != std::string::npos ||
             lower[i].find("missing") != std::string::npos)) {
            const std::string missing = quoted_after(line, "mod id:");
            const std::string requester = quoted_after(line, "requested by:");
            if (!missing.empty()) {
                auto finding = make_finding(
                    "dependency.missing", "dependency", "缺少模组依赖",
                    "缺少前置模组 " + missing +
                        (requester.empty() ? "。" : "，由 " + requester + " 请求。"),
                    "error");
                finding.mod_a = missing;
                finding.mod_b = requester;
                finding.suggestions = {
                    "安装前置模组 " + missing,
                    requester.empty()
                        ? "确认 Forge / NeoForge / Fabric API 等前置已安装"
                        : "检查 " + requester + " 支持的版本范围",
                    "安装完成后重启游戏"};
                finding.time = timestamp_near(lines, i);
                finding.event = clip(line, 600);
                finding.evidence = evidence_near(lines, i);
                add_finding(findings, seen, std::move(finding));
                dependency_found = true;
            }
        }

        if (lower[i].find("mod '") != std::string::npos &&
            lower[i].find(" requires ") != std::string::npos &&
            (lower[i].find("missing") != std::string::npos ||
             lower[i].find("unsupported") != std::string::npos)) {
            const std::string requester = quoted_after(line, "mod ");
            std::string missing;
            const size_t requires_pos = lower[i].find(" requires ");
            if (requires_pos != std::string::npos) {
                missing = quoted_after(line.substr(requires_pos + 10), "");
            }
            if (missing.empty()) {
                missing = quoted_after(line, "of mod ");
            }
            if (!requester.empty() && !missing.empty() &&
                requester != missing) {
                auto finding = make_finding(
                    "dependency.missing", "dependency", "缺少模组依赖",
                    "缺少模组 " + missing + "，由 " + requester + " 请求。",
                    "error");
                finding.mod_a = missing;
                finding.mod_b = requester;
                finding.suggestions = {"安装模组 " + missing,
                                       "确认 " + requester +
                                           " 与当前游戏版本匹配",
                                       "安装完成后重启游戏"};
                finding.time = timestamp_near(lines, i);
                finding.event = clip(line, 600);
                finding.evidence = evidence_near(lines, i);
                add_finding(findings, seen, std::move(finding));
                dependency_found = true;
            }
        }

        if (lower[i].find("missing dependency:") != std::string::npos) {
            const std::string missing = token_after(line, "missing dependency:");
            if (!missing.empty()) {
                auto finding = make_finding(
                    "dependency.missing", "dependency", "缺少模组依赖",
                    "加载器报告缺少依赖：" + missing + "。", "error");
                finding.mod_a = missing;
                finding.suggestions = {"安装模组 " + missing,
                                       "检查 mods 文件夹里的前置是否完整"};
                finding.time = timestamp_near(lines, i);
                finding.event = clip(line, 600);
                finding.evidence = evidence_near(lines, i);
                add_finding(findings, seen, std::move(finding));
                dependency_found = true;
            }
        }
    }
    if (!dependency_found) {
        if (const auto index = first_index_any(
                lower, {"missing or unsupported mandatory dependencies",
                        "modresolutionexception", "mod resolution failed"})) {
            auto finding = make_finding(
                "dependency.generic", "dependency", "缺少模组依赖",
                "有模组缺少前置或依赖未满足，加载器拒绝启动。", "error");
            finding.suggestions = {
                "按日志里 Missing dependency 的名字补装对应模组",
                "确认 Fabric API、NeoForge 等前置已经安装",
                "把缺少的模组放进 mods 后重试"};
            finding.time = timestamp_near(lines, *index);
            finding.event = clip(lines[*index], 600);
            finding.evidence = evidence_near(lines, *index);
            add_finding(findings, seen, std::move(finding));
        }
    }

    // ---- 加载器 / 版本不兼容 ----
    int loader_findings = 0;
    for (size_t i = 0; i < lower.size() && loader_findings < 3; ++i) {
        const bool incompatible =
            lower[i].find("incompatible mod set") != std::string::npos ||
            lower[i].find("requires minecraft version") != std::string::npos ||
            lower[i].find("requires forge version") != std::string::npos ||
            lower[i].find("requires neoforge version") != std::string::npos ||
            lower[i].find("requires fabric loader") != std::string::npos ||
            lower[i].find("requires fabric-loader") != std::string::npos ||
            lower[i].find("requires quilt loader") != std::string::npos ||
            lower[i].find("requires quilt-loader") != std::string::npos ||
            lower[i].find("is not compatible with") != std::string::npos ||
            lower[i].find("not compatible with this version") !=
                std::string::npos;
        if (!incompatible) continue;

        std::string mod = quoted_after(lines[i], "mod ");
        if (mod.empty()) {
            const auto quotes = quoted_strings(lines[i]);
            if (!quotes.empty()) mod = quotes.front();
        }
        auto finding = make_finding(
            "loader.incompatible", "loader", "模组加载器 / 版本不兼容",
            "模组或加载器版本与当前游戏不匹配。" +
                (mod.empty() ? std::string{} : " 涉及模组：" + mod + "。"),
            "error");
        finding.mod_a = mod;
        finding.suggestions = {
            "确认所有模组支持当前 Minecraft 版本",
            "检查 Forge / NeoForge / Fabric / Quilt 的版本要求",
            "移除或更新提示中的模组"};
        finding.time = timestamp_near(lines, i);
        finding.event = clip(lines[i], 600);
        finding.evidence = evidence_near(lines, i);
        add_finding(findings, seen, std::move(finding));
        ++loader_findings;
    }

    // ---- 已知冲突组合 ----
    const auto optifine = first_index(lower, "optifine");
    const auto sodium = first_index_any(
        lower, {"sodium", "embedium", "rubidium", "magnesium"});
    if (exit_code != 0 && optifine && sodium) {
        const size_t index = std::min(*optifine, *sodium);
        auto finding = make_finding(
            "conflict.optifine_render", "conflict",
            "OptiFine 与渲染优化模组冲突",
            "日志中同时出现 OptiFine 和 Sodium / Embeddium 系列模组。它们都会"
            "修改渲染管线，同时安装会导致类冲突、黑屏或启动崩溃。",
            "error");
        finding.mod_a = "OptiFine";
        finding.mod_b = "Sodium / Embeddium";
        finding.suggestions = {
            "只保留其中一套：OptiFine 或 Sodium/Embeddium + Iris",
            "不要同时安装 OptiFine 和 Sodium/Embeddium",
            "如果整合包要求 OptiFine，移除 Sodium/Embeddium 及其附属"};
        finding.time = timestamp_near(lines, index);
        finding.event = clip(lines[index], 600);
        finding.evidence = evidence_near(lines, index);
        add_finding(findings, seen, std::move(finding));
    }
    const auto iris = first_index(lower, "iris");
    if (exit_code != 0 && optifine && iris) {
        const size_t index = std::min(*optifine, *iris);
        auto finding = make_finding(
            "conflict.optifine_iris", "conflict", "OptiFine 与 Iris 冲突",
            "OptiFine 和 Iris 都会替换渲染管线，通常不能同时使用。", "error");
        finding.mod_a = "OptiFine";
        finding.mod_b = "Iris";
        finding.suggestions = {"使用 Iris + Sodium，或单独使用 OptiFine",
                               "移除其中一个后重试"};
        finding.time = timestamp_near(lines, index);
        finding.event = clip(lines[index], 600);
        finding.evidence = evidence_near(lines, index);
        add_finding(findings, seen, std::move(finding));
    }

    // ---- 重复模组 ----
    for (size_t i = 0; i < lower.size(); ++i) {
        if (lower[i].find("duplicate mod") == std::string::npos) continue;
        std::string mod = token_after(lines[i], "duplicate mods:");
        if (mod.empty()) mod = token_after(lines[i], "duplicate mod:");
        auto finding = make_finding(
            "conflict.duplicate", "conflict", "重复安装的模组",
            "加载器检测到重复的模组文件，通常是把同一个模组的不同版本同时放进了"
            " mods 文件夹。" +
                (mod.empty() ? std::string{} : " 涉及模组：" + mod + "。"),
            "error");
        finding.mod_a = mod;
        finding.suggestions = {
            "检查 mods 文件夹，删除重复或旧版本的模组",
            "只保留与当前游戏版本匹配的一个版本"};
        finding.time = timestamp_near(lines, i);
        finding.event = clip(lines[i], 600);
        finding.evidence = evidence_near(lines, i);
        add_finding(findings, seen, std::move(finding));
        break;
    }

    // ---- Mixin 冲突 ----
    int mixin_findings = 0;
    for (size_t i = 0; i < lower.size() && mixin_findings < 3; ++i) {
        const bool mixin =
            lower[i].find("mixin apply for mod") != std::string::npos ||
            lower[i].find("mixin apply failed") != std::string::npos ||
            lower[i].find("invalidmixin") != std::string::npos ||
            lower[i].find("mixintransformererror") != std::string::npos ||
            lower[i].find("mixin transformer") != std::string::npos;
        if (!mixin) continue;

        std::string mod = token_after(lines[i], "mixin apply for mod");
        if (mod.empty()) mod = quoted_after(lines[i], "mod ");
        if (mod.empty()) {
            const size_t marker = lower[i].find("mixins.");
            if (marker != std::string::npos) {
                const size_t begin = marker + 7;
                const size_t end = lower[i].find('.', begin);
                if (end != std::string::npos && end > begin) {
                    mod = clip(lines[i].substr(begin, end - begin), 90);
                }
            }
        }
        auto finding = make_finding(
            "mixin.failed", "mixin", "Mixin 注入失败",
            "模组在修改游戏类时发生冲突，常见于两个模组修改同一个类，或模组版本"
            "与游戏版本不匹配。" +
                (mod.empty() ? std::string{} : " 涉及模组：" + mod + "。"),
            "error");
        finding.mod_a = mod;
        finding.suggestions = {
            "先移除最近添加或更新的模组",
            mod.empty() ? "查看日志里的 Mixin config 名称"
                        : "更新或移除模组 " + mod,
            "确认模组之间没有功能重叠"};
        finding.time = timestamp_near(lines, i);
        finding.event = clip(lines[i], 600);
        finding.evidence = evidence_near(lines, i);
        add_finding(findings, seen, std::move(finding));
        ++mixin_findings;
    }

    // ---- 显式冲突描述 ----
    for (size_t i = 0; i < lower.size(); ++i) {
        const bool explicit_conflict =
            lower[i].find("is incompatible with") != std::string::npos ||
            lower[i].find("conflicting mods") != std::string::npos ||
            lower[i].find("conflict between") != std::string::npos ||
            lower[i].find("conflicts with") != std::string::npos;
        if (!explicit_conflict) continue;

        const auto quotes = quoted_strings(lines[i]);
        auto finding = make_finding(
            "conflict.explicit", "conflict", "模组之间发生冲突",
            "日志明确报告了两个模组不兼容。" +
                (quotes.size() >= 2
                     ? " 涉及：" + quotes[0] + " 与 " + quotes[1] + "。"
                     : std::string{}),
            "error");
        if (quotes.size() >= 2) {
            finding.mod_a = quotes[0];
            finding.mod_b = quotes[1];
        }
        finding.suggestions = {
            "移除或替换其中一个模组",
            "查看模组页面上的兼容性说明",
            "如果整合包要求同时安装，检查是否有兼容补丁"};
        finding.time = timestamp_near(lines, i);
        finding.event = clip(lines[i], 600);
        finding.evidence = evidence_near(lines, i);
        add_finding(findings, seen, std::move(finding));
        break;
    }

    // ---- 模组加载阶段异常 ----
    int mod_crashes = 0;
    for (size_t i = 0; i < lower.size() && mod_crashes < 3; ++i) {
        if (lower[i].find("caught exception from") == std::string::npos) {
            continue;
        }
        const std::string mod = token_after(lines[i], "caught exception from");
        auto finding = make_finding(
            "mod.caught_exception", "mod_crash", "模组加载失败",
            "模组 " + (mod.empty() ? std::string("（未知）") : mod) +
                " 在加载阶段抛出了异常。" + " " + clip(lines[i], 240),
            "error");
        finding.mod_a = mod;
        finding.suggestions = {
            mod.empty() ? "查看崩溃报告里的 Caught exception from"
                        : "先移除或更新模组 " + mod,
            "打开 crash-reports 里最新文件，查看 Caused by 的具体原因",
            "确认该模组版本与当前游戏版本匹配"};
        finding.time = timestamp_near(lines, i);
        finding.event = clip(lines[i], 600);
        finding.evidence = evidence_near(lines, i);
        add_finding(findings, seen, std::move(finding));
        ++mod_crashes;
    }

    // ---- 模组版本不匹配 / 缺类 ----
    if (const auto index = first_index_any(
            lower, {"nosuchmethoderror", "nosuchfielderror",
                    "abstractmethoderror", "incompatibleclasschangeerror",
                    "verifyerror"})) {
        std::string symbol = token_after(lines[*index], "error:");
        if (symbol.empty()) symbol = token_after(lines[*index], "error");
        auto finding = make_finding(
            "mod.version_mismatch", "version", "模组版本不匹配",
            "模组调用了当前游戏或加载器里不存在的类/方法。通常是对应模组版本"
            "与游戏版本不一致。" +
                (symbol.empty() ? std::string{} : " 符号：" + symbol + "。"),
            "error");
        finding.mod_a = symbol;
        finding.suggestions = {
            "移除最近添加或更新的模组",
            "确认所有模组都支持当前游戏版本",
            "查看崩溃报告里 NoSuchMethod / NoSuchField 提到的类名"};
        finding.time = timestamp_near(lines, *index);
        finding.event = clip(lines[*index], 600);
        finding.evidence = evidence_near(lines, *index);
        add_finding(findings, seen, std::move(finding));
    }
    if (const auto index = first_index_any(
            lower, {"noclassdeffounderror", "classnotfoundexception"})) {
        std::string missing_class = token_after(lines[*index], "error:");
        if (missing_class.empty()) {
            missing_class = token_after(lines[*index], "exception:");
        }
        auto finding = make_finding(
            "dependency.missing_class", "dependency", "缺少运行库或模组前置",
            "游戏找不到某个类。" +
                (missing_class.empty()
                     ? std::string{}
                     : " 缺失的类：" + missing_class + "。"),
            "error");
        finding.mod_a = missing_class;
        finding.suggestions = {
            "补装日志里提到的类所属的模组或前置",
            "重新安装该版本",
            "确认 mods 文件夹里的依赖完整"};
        finding.time = timestamp_near(lines, *index);
        finding.event = clip(lines[*index], 600);
        finding.evidence = evidence_near(lines, *index);
        add_finding(findings, seen, std::move(finding));
    }

    // ---- 原生库 ----
    if (const auto index = first_index_any(
            lower, {"unsatisfiedlinkerror", "can't find dependent libraries",
                    "no lwjgl in java.library.path",
                    "failed to load native library"})) {
        auto finding = make_finding(
            "native.library", "native", "原生库加载失败",
            "Java 无法加载 LWJGL 或其它原生库（.dll）。", "error");
        finding.suggestions = {
            "使用 64 位 Java",
            "删除 versions/<版本>/natives 后重新启动，让启动器重新释放原生库",
            "更新显卡驱动"};
        finding.time = timestamp_near(lines, *index);
        finding.event = clip(lines[*index], 600);
        finding.evidence = evidence_near(lines, *index);
        add_finding(findings, seen, std::move(finding));
    }

    // ---- JVM 崩溃 ----
    if (const auto index = first_index_any(
            lower, {"exception_access_violation",
                    "a fatal error has been detected by the java runtime "
                    "environment",
                    "sigsegv", "hs_err_pid"})) {
        auto finding = make_finding(
            "jvm.crash", "jvm", "Java 虚拟机崩溃",
            "Java 本身崩溃了，不是普通的模组异常。", "error");
        finding.suggestions = {
            "更新显卡驱动", "暂时移除 OptiFine、光影或最近添加的模组",
            "查看游戏目录下的 hs_err_pid*.log",
            "如果反复出现，检查内存/CPU 是否超频"};
        finding.time = timestamp_near(lines, *index);
        finding.event = clip(lines[*index], 600);
        finding.evidence = evidence_near(lines, *index);
        add_finding(findings, seen, std::move(finding));
    }

    // ---- 显卡 / OpenGL ----
    if (const auto index = first_index_any(
            lower, {"pixel format not accelerated", "glfw error",
                    "failed to create window", "could not create gl context",
                    "failed to initialize gl", "opengl error"})) {
        auto finding = make_finding(
            "graphics.opengl", "graphics", "显卡初始化失败",
            "游戏无法创建 OpenGL 上下文或窗口。", "error");
        finding.suggestions = {
            "更新显卡驱动", "不要通过远程桌面或虚拟机启动",
            "关闭光影/OptiFine，或切换到兼容模式"};
        finding.time = timestamp_near(lines, *index);
        finding.event = clip(lines[*index], 600);
        finding.evidence = evidence_near(lines, *index);
        add_finding(findings, seen, std::move(finding));
    }

    // ---- 崩溃报告 ----
    if (const auto index = first_index_any(
            lower, {"the game crashed whilst", "game crashed",
                    "crash report saved"})) {
        std::string detail = "游戏生成了崩溃报告。";
        for (size_t i = *index; i < lines.size() && i < *index + 8; ++i) {
            if (lower[i].find("description:") != std::string::npos ||
                lower[i].find("error:") != std::string::npos) {
                detail += " " + clip(lines[i], 240);
                break;
            }
        }
        auto finding = make_finding("crash.report", "crash", "游戏崩溃",
                                    detail, "error");
        finding.suggestions = {
            "打开 .minecraft/crash-reports 里最新的文件",
            "查看报告里的 Caused by 部分",
            "先移除最近添加的模组再试"};
        finding.time = timestamp_near(lines, *index);
        finding.event = clip(lines[*index], 600);
        finding.evidence = evidence_near(lines, *index);
        add_finding(findings, seen, std::move(finding));
    }

    if (const auto index = first_index(lower, "exception in thread \"main\"")) {
        auto finding = make_finding(
            "crash.main_thread", "crash", "游戏因异常退出",
            clip(lines[*index], 300), "error");
        finding.suggestions = {
            "查看 .minecraft/logs/latest.log 最后一段",
            "移除最近添加的模组",
            "把崩溃报告发给整合包作者"};
        finding.time = timestamp_near(lines, *index);
        finding.event = clip(lines[*index], 600);
        finding.evidence = evidence_near(lines, *index);
        add_finding(findings, seen, std::move(finding));
    }

    if (findings.empty()) {
        if (exit_code == 0) {
            auto summary = normal_summary(offline_auth);
            summary.context = context;
            return summary;
        }
        RunSummary summary;
        summary.context = context;
        summary.title = "游戏异常退出";
        summary.detail =
            exit_hint(exit_code) + " 日志里没有匹配到常见错误特征。";
        summary.suggestions = {
            "打开 .minecraft/logs/latest.log，查看最后的红色错误",
            "打开 .minecraft/crash-reports 里最新的文件",
            "先把最近添加的模组移出 mods 再试"};
        summary.severity = "error";
        summary.unknown = true;
        return summary;
    }

    RunSummary summary;
    summary.context = context;
    summary.findings = std::move(findings);
    const AnalysisFinding& primary = summary.findings.front();
    summary.title = primary.title;
    summary.detail = primary.detail;
    summary.suggestions = primary.suggestions;
    summary.severity = primary.severity;
    summary.normal = exit_code == 0;
    // 游戏正常退出时即使日志里有错误，也不要显示成“异常退出”。
    if (summary.normal && summary.severity == "error") {
        summary.severity = "warn";
    }

    if (summary.findings.size() > 1) {
        summary.detail += " 本次共发现 " +
                          std::to_string(summary.findings.size()) +
                          " 个可能的问题。";
    }
    return summary;
}

std::vector<std::string> read_log_tail(const std::filesystem::path& path,
                                       size_t max_lines) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};

    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size <= 0) return {};

    constexpr std::streamoff kMaxBytes = 256 * 1024;
    const std::streamoff start = std::max<std::streamoff>(0, size - kMaxBytes);
    file.seekg(start);

    std::string text((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    if (start > 0) {
        const size_t newline = text.find('\n');
        if (newline != std::string::npos) text.erase(0, newline + 1);
    }

    std::vector<std::string> lines;
    size_t begin = 0;
    while (begin <= text.size()) {
        size_t end = text.find('\n', begin);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(begin, end - begin);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(std::move(line));
        if (end == text.size()) break;
        begin = end + 1;
    }

    if (lines.size() > max_lines) {
        lines.erase(lines.begin(), lines.begin() +
                                       static_cast<std::ptrdiff_t>(
                                           lines.size() - max_lines));
    }
    return lines;
}

std::string format_run_summary(const RunSummary& summary) {
    std::string text = "运行结果：" + summary.title + "\n";
    text += "原因：" + summary.detail + "\n";
    if (!summary.findings.empty()) {
        text += "崩溃分析：\n";
        for (const auto& finding : summary.findings) {
            text += "  [" + finding.category + "] " + finding.title;
            if (!finding.time.empty()) text += "（" + finding.time + "）";
            text += "\n";
            text += "    " + finding.detail + "\n";
            if (!finding.mod_a.empty() || !finding.mod_b.empty()) {
                text += "    涉及模组：" + finding.mod_a;
                if (!finding.mod_b.empty()) text += " ↔ " + finding.mod_b;
                text += "\n";
            }
        }
    }
    if (!summary.suggestions.empty()) {
        text += "建议：\n";
        for (size_t i = 0; i < summary.suggestions.size(); ++i) {
            text += "  " + std::to_string(i + 1) + ". " +
                    summary.suggestions[i] + "\n";
        }
    }
    return text;
}

} // namespace cl::mc
