#include "mc/version.h"

#include <windows.h>
#include <winternl.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <sstream>

#include "core/encoding.h"
#include "core/log.h"

namespace cl::mc {

namespace {

std::string string_or(const nlohmann::json& json, const char* key,
                      std::string fallback = {}) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

std::optional<std::string> optional_string(const nlohmann::json& json,
                                           const char* key) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_string()) return std::nullopt;
    return it->get<std::string>();
}

DownloadInfo parse_download(const nlohmann::json& json) {
    DownloadInfo info;
    info.path = string_or(json, "path");
    info.url = string_or(json, "url");
    info.sha1 = string_or(json, "sha1");
    const auto size = json.find("size");
    if (size != json.end()) {
        if (size->is_number_unsigned()) {
            info.size = size->get<unsigned long long>();
        } else if (size->is_number_integer()) {
            const long long value = size->get<long long>();
            if (value > 0) info.size = static_cast<unsigned long long>(value);
        }
    }
    return info;
}

std::vector<Rule> parse_rules(const nlohmann::json& json) {
    std::vector<Rule> rules;
    if (!json.is_array()) return rules;

    for (const auto& item : json) {
        if (!item.is_object()) continue;
        Rule rule;
        rule.action = string_or(item, "action");
        if (const auto os = item.find("os"); os != item.end() && os->is_object()) {
            RuleOs parsed;
            parsed.name = optional_string(*os, "name");
            parsed.arch = optional_string(*os, "arch");
            parsed.version = optional_string(*os, "version");
            rule.os = std::move(parsed);
        }
        if (const auto features = item.find("features");
            features != item.end() && features->is_object()) {
            std::map<std::string, bool> parsed;
            for (auto it = features->begin(); it != features->end(); ++it) {
                if (it.value().is_boolean()) parsed[it.key()] = it.value().get<bool>();
            }
            rule.features = std::move(parsed);
        }
        if (!rule.action.empty()) rules.push_back(std::move(rule));
    }
    return rules;
}

std::vector<ArgItem> parse_arg_array(const nlohmann::json& json) {
    std::vector<ArgItem> result;
    if (!json.is_array()) return result;

    for (const auto& item : json) {
        if (item.is_string()) {
            result.push_back(ArgItem{item.get<std::string>(), {}, false});
            continue;
        }
        if (!item.is_object()) continue;

        const bool has_rules = item.contains("rules");
        std::vector<Rule> rules;
        if (has_rules) rules = parse_rules(item["rules"]);

        const auto value = item.find("value");
        if (value == item.end()) continue;
        if (value->is_string()) {
            result.push_back(ArgItem{value->get<std::string>(), rules, has_rules});
        } else if (value->is_array()) {
            for (const auto& entry : *value) {
                if (entry.is_string()) {
                    result.push_back(ArgItem{entry.get<std::string>(), rules, has_rules});
                }
            }
        }
    }
    return result;
}

std::string os_version_string() {
    static const std::string version = [] {
        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdll != nullptr) {
            const auto fn = reinterpret_cast<RtlGetVersionFn>(
                ::GetProcAddress(ntdll, "RtlGetVersion"));
            if (fn != nullptr) {
                RTL_OSVERSIONINFOW info{};
                info.dwOSVersionInfoSize = sizeof(info);
                if (fn(&info) == 0) {
                    return std::to_string(info.dwMajorVersion) + "." +
                           std::to_string(info.dwMinorVersion) + "." +
                           std::to_string(info.dwBuildNumber);
                }
            }
        }
        return std::string("10.0.0");
    }();
    return version;
}

bool feature_value(const FeatureSet& features, const std::string& key) {
    if (key == "is_demo_user") return features.demo_user;
    if (key == "has_custom_resolution") return features.custom_resolution;
    if (key == "has_quick_plays_support") return features.quick_play_support;
    if (key == "is_quick_play_singleplayer") return features.quick_play_singleplayer;
    if (key == "is_quick_play_multiplayer") return features.quick_play_multiplayer;
    if (key == "is_quick_play_realms") return features.quick_play_realms;
    return false;
}

bool rule_matches(const Rule& rule, const FeatureSet& features) {
    if (rule.os) {
        if (rule.os->name) {
            const std::string& name = *rule.os->name;
            // 我们是 Windows 64 位客户端。
            if (name != "windows" && name != "unknown") return false;
        }
        if (rule.os->arch) {
            const std::string& arch = *rule.os->arch;
            if (arch == "x86") return false;
            if (arch.find("arm") != std::string::npos) return false;
        }
        if (rule.os->version) {
            try {
                if (!std::regex_match(os_version_string(), std::regex(*rule.os->version))) {
                    return false;
                }
            } catch (const std::regex_error&) {
                Log::warn("版本 JSON 中的 os.version 正则无效：" + *rule.os->version);
                return false;
            }
        }
    }
    if (rule.features) {
        for (const auto& [key, expected] : *rule.features) {
            if (feature_value(features, key) != expected) return false;
        }
    }
    return true;
}

// 与 Newtonsoft.Json 的 JObject.Merge 默认行为一致：
// 对象递归合并，其余类型（包括数组）由 source 覆盖。
void merge_json(nlohmann::json& target, const nlohmann::json& source) {
    if (!target.is_object() || !source.is_object()) {
        target = source;
        return;
    }
    for (auto it = source.begin(); it != source.end(); ++it) {
        const auto existing = target.find(it.key());
        if (existing != target.end() && existing->is_object() &&
            it.value().is_object()) {
            merge_json(*existing, it.value());
        } else {
            target[it.key()] = it.value();
        }
    }
}

void replace_all_in_place(std::string& text, const std::string& from,
                          const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

} // namespace

bool rules_allow(const std::vector<Rule>& rules, const FeatureSet& features) {
    if (rules.empty()) return true;
    bool allowed = false;
    for (const Rule& rule : rules) {
        if (!rule_matches(rule, features)) continue;
        allowed = rule.action == "allow";
    }
    return allowed;
}

std::string MavenCoord::relative_path(
    const std::optional<std::string>& classifier_override) const {
    std::string group_path = group;
    std::replace(group_path.begin(), group_path.end(), '.', '/');

    std::string file = artifact + "-" + version;
    const std::string& cls = classifier_override ? *classifier_override : classifier;
    if (!cls.empty()) file += "-" + cls;
    file += "." + (extension.empty() ? "jar" : extension);

    return group_path + "/" + artifact + "/" + version + "/" + file;
}

std::optional<MavenCoord> parse_maven(std::string_view name) {
    std::string text(name);
    MavenCoord coord;

    if (const size_t at = text.rfind('@'); at != std::string::npos) {
        coord.extension = text.substr(at + 1);
        text.erase(at);
    }

    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t pos = text.find(':', start);
        parts.push_back(text.substr(start, pos == std::string::npos ? pos : pos - start));
        if (pos == std::string::npos) break;
        start = pos + 1;
    }

    if (parts.size() < 3 || parts.size() > 4) return std::nullopt;
    coord.group = parts[0];
    coord.artifact = parts[1];
    coord.version = parts[2];
    if (parts.size() == 4) coord.classifier = parts[3];
    if (coord.group.empty() || coord.artifact.empty() || coord.version.empty()) {
        return std::nullopt;
    }
    return coord;
}

std::string Library::artifact_path() const {
    if (artifact && !artifact->path.empty()) return artifact->path;
    const auto coord = parse_maven(name);
    if (!coord) return {};
    return coord->relative_path();
}

std::string Library::artifact_url() const {
    if (artifact && !artifact->url.empty()) return artifact->url;
    std::string base = base_url;
    if (!base.empty() && base.back() != '/') base.push_back('/');
    return base + artifact_path();
}

std::optional<std::string> Library::natives_profile(const std::string& os) const {
    const auto it = natives.find(os);
    if (it == natives.end() || it->second.empty()) return std::nullopt;
    std::string classifier = it->second;
    replace_all_in_place(classifier, "${arch}", "64");
    return classifier;
}

std::optional<DownloadInfo> Library::natives_artifact() const {
    const auto classifier = natives_profile("windows");
    if (!classifier) return std::nullopt;

    if (const auto it = classifiers.find(*classifier); it != classifiers.end()) {
        return it->second;
    }

    const auto coord = parse_maven(name);
    if (!coord) return std::nullopt;

    DownloadInfo info;
    info.path = coord->relative_path(*classifier);
    std::string base = base_url;
    if (!base.empty() && base.back() != '/') base.push_back('/');
    info.url = base + info.path;
    return info;
}

Version parse_version(const nlohmann::json& json) {
    Version version;
    version.id = string_or(json, "id");
    version.type = string_or(json, "type");
    version.inherits_from = optional_string(json, "inheritsFrom");
    version.jar = optional_string(json, "jar");
    version.main_class = optional_string(json, "mainClass");
    version.assets = optional_string(json, "assets");
    version.minecraft_arguments = optional_string(json, "minecraftArguments");

    if (const auto asset_index = json.find("assetIndex");
        asset_index != json.end() && asset_index->is_object()) {
        version.asset_index_id = optional_string(*asset_index, "id");
        if (const auto url = optional_string(*asset_index, "url");
            url && !url->empty()) {
            version.asset_index = parse_download(*asset_index);
            version.asset_index->url = *url;
        }
    }
    if (!version.asset_index_id) version.asset_index_id = version.assets;

    if (const auto downloads = json.find("downloads");
        downloads != json.end() && downloads->is_object()) {
        if (const auto client = downloads->find("client");
            client != downloads->end() && client->is_object()) {
            version.client = parse_download(*client);
        }
    }

    if (const auto java_version = json.find("javaVersion");
        java_version != json.end() && java_version->is_object()) {
        if (const auto major = java_version->find("majorVersion");
            major != java_version->end() && major->is_number_integer()) {
            version.java_major = major->get<int>();
        }
    }

    if (const auto libraries = json.find("libraries");
        libraries != json.end() && libraries->is_array()) {
        for (const auto& entry : *libraries) {
            // 某些第三方启动器会把空项序列化成 null。
            if (!entry.is_object()) continue;

            Library library;
            library.name = string_or(entry, "name");
            if (library.name.empty()) continue;
            if (const auto url = optional_string(entry, "url"); url && !url->empty()) {
                library.base_url = *url;
            }
            if (entry.contains("rules")) library.rules = parse_rules(entry["rules"]);

            if (const auto downloads = entry.find("downloads");
                downloads != entry.end() && downloads->is_object()) {
                if (const auto artifact = downloads->find("artifact");
                    artifact != downloads->end() && artifact->is_object()) {
                    library.artifact = parse_download(*artifact);
                }
                if (const auto classifiers = downloads->find("classifiers");
                    classifiers != downloads->end() && classifiers->is_object()) {
                    for (auto it = classifiers->begin(); it != classifiers->end(); ++it) {
                        if (it.value().is_object()) {
                            library.classifiers[it.key()] = parse_download(it.value());
                        }
                    }
                }
            }

            if (const auto natives = entry.find("natives");
                natives != entry.end() && natives->is_object()) {
                for (auto it = natives->begin(); it != natives->end(); ++it) {
                    if (it.value().is_string()) {
                        library.natives[it.key()] = it.value().get<std::string>();
                    }
                }
            }

            if (const auto extract = entry.find("extract");
                extract != entry.end() && extract->is_object()) {
                if (const auto exclude = extract->find("exclude");
                    exclude != extract->end() && exclude->is_array()) {
                    for (const auto& item : *exclude) {
                        if (item.is_string()) {
                            library.extract_exclude.push_back(item.get<std::string>());
                        }
                    }
                }
            }

            version.libraries.push_back(std::move(library));
        }
    }

    if (const auto arguments = json.find("arguments");
        arguments != json.end() && arguments->is_object()) {
        if (const auto jvm = arguments->find("jvm"); jvm != arguments->end()) {
            version.jvm_args = parse_arg_array(*jvm);
        }
        if (const auto game = arguments->find("game"); game != arguments->end()) {
            version.game_args = parse_arg_array(*game);
        }
    }

    return version;
}

Result<nlohmann::json> load_json(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return Error::make(1, "无法打开版本 JSON", path_to_utf8(path));
    }
    std::string text((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());

    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }

    try {
        return nlohmann::json::parse(text);
    } catch (const std::exception& ex) {
        return Error::make(2, "版本 JSON 解析失败",
                           path_to_utf8(path) + ": " + ex.what());
    }
}

Result<ResolvedVersion> resolve_version(const std::filesystem::path& versions_dir,
                                        const std::string& id) {
    const std::filesystem::path id_path = path_from_utf8(id);
    const std::filesystem::path version_dir = versions_dir / id_path;
    const std::filesystem::path json_path =
        version_dir / (id_path.wstring() + L".json");

    std::vector<nlohmann::json> chain_json;
    std::vector<std::string> ids;
    std::set<std::string> seen;
    std::string current = id;

    for (int depth = 0; depth < 16; ++depth) {
        if (seen.contains(current)) {
            return Error::make(3, "版本继承出现循环", current);
        }
        seen.insert(current);

        const std::filesystem::path current_path = path_from_utf8(current);
        auto loaded = load_json(versions_dir / current_path /
                                (current_path.wstring() + L".json"));
        if (!loaded) return loaded.error();

        nlohmann::json json = std::move(loaded).value();
        if (!json.is_object()) {
            return Error::make(4, "版本 JSON 不是对象", current);
        }

        const std::string next = string_or(json, "inheritsFrom");
        chain_json.push_back(std::move(json));
        ids.push_back(current);

        if (next.empty() || next == current) break;
        current = next;
    }

    // 与 PCL 一致的合并方式：
    // 子版本的 libraries 排在前，父版本排在后；其余字段子版本优先。
    nlohmann::json merged = chain_json.front();
    for (size_t i = 1; i < chain_json.size(); ++i) {
        nlohmann::json parent = chain_json[i];
        nlohmann::json libraries = nlohmann::json::array();
        if (merged.contains("libraries") && merged["libraries"].is_array()) {
            for (const auto& item : merged["libraries"]) libraries.push_back(item);
        }
        if (parent.contains("libraries") && parent["libraries"].is_array()) {
            for (const auto& item : parent["libraries"]) libraries.push_back(item);
        }
        merge_json(parent, merged);
        parent["libraries"] = std::move(libraries);
        merged = std::move(parent);
    }

    try {
        ResolvedVersion resolved;
        resolved.id = id;
        resolved.vanilla_id = ids.back();
        resolved.version_dir = version_dir;
        resolved.merged = parse_version(merged);
        resolved.merged.id = id;
        for (const auto& item : chain_json) {
            resolved.chain.push_back(parse_version(item));
        }

        resolved.jar_id = resolved.vanilla_id;
        for (const auto& item : chain_json) {
            if (const auto jar = optional_string(item, "jar"); jar && !jar->empty()) {
                resolved.jar_id = *jar;
                break;
            }
        }
        return resolved;
    } catch (const std::exception& ex) {
        return Error::make(5, "解析版本 JSON 结构失败",
                           std::string(ex.what()) + " @ " + path_to_utf8(json_path));
    }
}

std::vector<std::string> split_legacy_args(std::string_view text) {
    std::vector<std::string> result;
    std::string current;
    bool in_quotes = false;
    bool has_token = false;

    for (const char c : text) {
        if (c == '"') {
            in_quotes = !in_quotes;
            has_token = true;
        } else if (std::isspace(static_cast<unsigned char>(c)) && !in_quotes) {
            if (has_token || !current.empty()) {
                result.push_back(current);
                current.clear();
                has_token = false;
            }
        } else {
            current.push_back(c);
            has_token = true;
        }
    }
    if (has_token || !current.empty()) result.push_back(current);
    return result;
}

std::string apply_placeholders(
    std::string text, const std::map<std::string, std::string>& placeholders) {
    for (const auto& [key, value] : placeholders) {
        replace_all_in_place(text, key, value);
    }
    return text;
}

std::vector<std::string> expand_args(
    const std::vector<ArgItem>& items, const FeatureSet& features,
    const std::map<std::string, std::string>& placeholders) {
    std::vector<std::string> result;
    for (const ArgItem& item : items) {
        if (item.conditional && !rules_allow(item.rules, features)) continue;
        result.push_back(apply_placeholders(item.value, placeholders));
    }
    return result;
}

} // namespace cl::mc
