#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/result.h"

namespace cl::mc {

struct RuleOs {
    std::optional<std::string> name;
    std::optional<std::string> arch;
    std::optional<std::string> version;
};

struct Rule {
    std::string action;  // "allow" / "disallow"
    std::optional<RuleOs> os;
    std::optional<std::map<std::string, bool>> features;
};

struct FeatureSet {
    bool demo_user = false;
    bool custom_resolution = false;
    bool quick_play_support = false;
    bool quick_play_singleplayer = false;
    bool quick_play_multiplayer = false;
    bool quick_play_realms = false;
};

bool rules_allow(const std::vector<Rule>& rules, const FeatureSet& features);

struct DownloadInfo {
    std::string path;
    std::string url;
    std::string sha1;
    unsigned long long size = 0;
};

struct MavenCoord {
    std::string group;
    std::string artifact;
    std::string version;
    std::string classifier;
    std::string extension = "jar";

    // group/artifact/version/artifact-version[-classifier].ext
    [[nodiscard]] std::string relative_path(
        const std::optional<std::string>& classifier_override = std::nullopt) const;
};

std::optional<MavenCoord> parse_maven(std::string_view name);

struct Library {
    std::string name;
    std::string base_url = "https://libraries.minecraft.net/";
    std::vector<Rule> rules;
    std::optional<DownloadInfo> artifact;
    std::map<std::string, DownloadInfo> classifiers;
    std::map<std::string, std::string> natives;  // os -> classifier
    std::vector<std::string> extract_exclude;

    [[nodiscard]] std::string artifact_path() const;
    [[nodiscard]] std::string artifact_url() const;
    [[nodiscard]] std::optional<std::string> natives_profile(
        const std::string& os) const;
    [[nodiscard]] std::optional<DownloadInfo> natives_artifact() const;
};

struct ArgItem {
    std::string value;
    std::vector<Rule> rules;
    bool conditional = false;
};

struct Version {
    std::string id;
    std::string type;
    std::optional<std::string> inherits_from;
    std::optional<std::string> jar;
    std::optional<std::string> main_class;
    std::optional<std::string> assets;
    std::optional<std::string> asset_index_id;
    std::optional<DownloadInfo> asset_index;
    std::optional<DownloadInfo> client;
    std::optional<int> java_major;
    std::vector<Library> libraries;
    std::vector<ArgItem> jvm_args;
    std::vector<ArgItem> game_args;
    std::optional<std::string> minecraft_arguments;
};

Version parse_version(const nlohmann::json& json);

Result<nlohmann::json> load_json(const std::filesystem::path& path);

struct ResolvedVersion {
    std::string id;         // 实例名（versions 下的文件夹名）
    std::string vanilla_id; // 继承链最底层的原版版本号
    std::string jar_id;     // 主 jar 所在版本
    Version merged;
    std::vector<Version> chain; // 子 -> 父
    std::filesystem::path version_dir;
};

// 读取 versions/<id>/<id>.json，按 PCL 的规则合并 inheritsFrom 链：
//   - libraries：子版本在前，父版本在后（Java 8 的 classpath 取第一个匹配）
//   - 其余字段：子版本覆盖父版本
Result<ResolvedVersion> resolve_version(const std::filesystem::path& versions_dir,
                                        const std::string& id);

std::vector<std::string> split_legacy_args(std::string_view text);

std::vector<std::string> expand_args(
    const std::vector<ArgItem>& items,
    const FeatureSet& features,
    const std::map<std::string, std::string>& placeholders);

std::string apply_placeholders(std::string text,
                               const std::map<std::string, std::string>& placeholders);

} // namespace cl::mc
