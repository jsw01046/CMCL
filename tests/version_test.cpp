#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "core/crypto.h"
#include "core/encoding.h"
#include "mc/version.h"

namespace fs = std::filesystem;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::cerr << "FAILED: " #condition "  (line " << __LINE__ << ")\n"; \
            return 1;                                                        \
        }                                                                    \
    } while (0)

namespace {

void write_file(const fs::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
}

const char* kParentJson = R"json({
  "id": "1.20.1",
  "type": "release",
  "mainClass": "net.minecraft.client.main.Main",
  "assetIndex": { "id": "5" },
  "assets": "5",
  "libraries": [
    {
      "name": "com.example:parent-lib:1.0",
      "downloads": {
        "artifact": {
          "path": "com/example/parent-lib/1.0/parent-lib-1.0.jar",
          "url": "https://example.com/parent.jar",
          "sha1": "00",
          "size": 3
        }
      }
    },
    {
      "name": "org.lwjgl:lwjgl:3.3.1",
      "downloads": {
        "artifact": { "path": "org/lwjgl/lwjgl/3.3.1/lwjgl-3.3.1.jar" },
        "classifiers": {
          "natives-windows": {
            "path": "org/lwjgl/lwjgl/3.3.1/lwjgl-3.3.1-natives-windows.jar",
            "url": "https://example.com/natives.jar"
          }
        }
      },
      "natives": { "windows": "natives-windows" }
    }
  ],
  "arguments": {
    "jvm": ["-cp", "${classpath}"],
    "game": ["--gameDir", "${game_directory}", "--username", "${auth_player_name}"]
  }
})json";

const char* kChildJson = R"json({
  "id": "forge-test",
  "inheritsFrom": "1.20.1",
  "type": "release",
  "mainClass": "cpw.mods.bootstraplauncher.BootstrapLauncher",
  "libraries": [
    {
      "name": "net.minecraftforge:forge:1.0",
      "downloads": {
        "artifact": { "path": "net/minecraftforge/forge/1.0/forge-1.0.jar" }
      }
    }
  ],
  "arguments": {
    "game": ["--launchTarget", "forgeclient"]
  }
})json";

} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "craftlauncher-version-test";
    std::error_code ec;
    fs::remove_all(root, ec);

    fs::create_directories(root / "versions" / "1.20.1", ec);
    fs::create_directories(root / "versions" / "forge-test", ec);
    write_file(root / "versions" / "1.20.1" / "1.20.1.json", kParentJson);
    write_file(root / "versions" / "forge-test" / "forge-test.json", kChildJson);

    auto resolved_result = cl::mc::resolve_version(root / "versions", "forge-test");
    CHECK(resolved_result.ok());
    const cl::mc::ResolvedVersion& resolved = resolved_result.value();

    CHECK(resolved.id == "forge-test");
    CHECK(resolved.vanilla_id == "1.20.1");
    CHECK(resolved.jar_id == "1.20.1");
    CHECK(resolved.chain.size() == 2);
    CHECK(resolved.chain[0].id == "forge-test");
    CHECK(resolved.chain[1].id == "1.20.1");

    // 子版本字段覆盖父版本。
    CHECK(resolved.merged.main_class.has_value());
    CHECK(*resolved.merged.main_class == "cpw.mods.bootstraplauncher.BootstrapLauncher");
    CHECK(resolved.merged.asset_index_id == "5");

    // libraries：子版本在前，父版本在后。
    CHECK(resolved.merged.libraries.size() == 3);
    CHECK(resolved.merged.libraries[0].name == "net.minecraftforge:forge:1.0");
    CHECK(resolved.merged.libraries[1].name == "com.example:parent-lib:1.0");
    CHECK(resolved.merged.libraries[2].name == "org.lwjgl:lwjgl:3.3.1");

    // 原生库分类。
    CHECK(resolved.merged.libraries[2].natives_profile("windows") == "natives-windows");
    CHECK(!resolved.merged.libraries[2].natives_profile("linux").has_value());
    const auto natives = resolved.merged.libraries[2].natives_artifact();
    CHECK(natives.has_value());
    CHECK(natives->path ==
          "org/lwjgl/lwjgl/3.3.1/lwjgl-3.3.1-natives-windows.jar");

    // Maven 坐标解析。
    const auto coord = cl::mc::parse_maven("org.lwjgl:lwjgl:3.3.1:natives-windows");
    CHECK(coord.has_value());
    CHECK(coord->relative_path() ==
          "org/lwjgl/lwjgl/3.3.1/lwjgl-3.3.1-natives-windows.jar");
    CHECK(coord->relative_path("natives-windows-arm64") ==
          "org/lwjgl/lwjgl/3.3.1/lwjgl-3.3.1-natives-windows-arm64.jar");

    // 参数按 子 -> 父 的顺序展开。
    const cl::mc::FeatureSet features;
    const std::map<std::string, std::string> placeholders = {
        {"${game_directory}", "C:/game"},
        {"${auth_player_name}", "Steve"},
    };
    const auto child_args =
        cl::mc::expand_args(resolved.chain[0].game_args, features, placeholders);
    CHECK(child_args.size() == 2);
    CHECK(child_args[0] == "--launchTarget");
    CHECK(child_args[1] == "forgeclient");
    const auto parent_args =
        cl::mc::expand_args(resolved.chain[1].game_args, features, placeholders);
    CHECK(parent_args.size() == 4);
    CHECK(parent_args[1] == "C:/game");
    CHECK(parent_args[3] == "Steve");

    // rules：有规则时默认 disallow；Linux 的 disallow 在 Windows 上不匹配。
    cl::mc::Rule linux_rule;
    linux_rule.action = "disallow";
    cl::mc::RuleOs linux_os;
    linux_os.name = "linux";
    linux_rule.os = linux_os;
    CHECK(!cl::mc::rules_allow({linux_rule}, features));

    cl::mc::Rule allow_all;
    allow_all.action = "allow";
    CHECK(cl::mc::rules_allow({allow_all, linux_rule}, features));

    cl::mc::Rule windows_rule;
    windows_rule.action = "allow";
    cl::mc::RuleOs windows_os;
    windows_os.name = "windows";
    windows_rule.os = windows_os;
    CHECK(cl::mc::rules_allow({windows_rule}, features));

    cl::mc::Rule arm_rule;
    arm_rule.action = "allow";
    cl::mc::RuleOs arm_os;
    arm_os.arch = "arm64";
    arm_rule.os = arm_os;
    CHECK(!cl::mc::rules_allow({arm_rule}, features));

    // 旧版参数分词。
    const auto legacy = cl::mc::split_legacy_args("--username \"a b\" --x");
    CHECK(legacy.size() == 3);
    CHECK(legacy[1] == "a b");

    // 离线 UUID 与 Java 的 nameUUIDFromBytes 格式一致。
    auto uuid = cl::offline_uuid("Steve");
    CHECK(uuid.ok());
    CHECK(uuid.value().size() == 36);
    CHECK(uuid.value()[14] == '3');

    // Java 7/8 中文日志是 GBK，读出来要转成 UTF-8。
    const std::string gbk("\xC3\xB0\xCF\xD5\xD5\xDF\xB4\xAB\xCB\xB5", 10);
    CHECK(cl::decode_to_utf8(gbk, cl::kCodePageGbk) == "冒险者传说");
    const std::string utf8_text = "冒险者传说";
    CHECK(cl::decode_to_utf8(utf8_text, cl::kCodePageGbk) == utf8_text);

    std::cout << "version_test: all checks passed\n";
    return 0;
}
