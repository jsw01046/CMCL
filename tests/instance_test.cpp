#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "mc/instance.h"

namespace fs = std::filesystem;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << "FAILED: " #condition "  (line " << __LINE__ << ")\n"; \
            return 1;                                                          \
        }                                                                      \
    } while (0)

namespace {

void write_file(const fs::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
}

nlohmann::json read_json(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    nlohmann::json json;
    file >> json;
    return json;
}

} // namespace

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root =
        fs::temp_directory_path() / ("craftlauncher_instance_test_" + std::to_string(stamp));
    std::error_code ec;
    fs::remove_all(root, ec);

    const fs::path versions = root / "versions";
    const fs::path a_dir = versions / "A";
    const fs::path b_dir = versions / "B";
    fs::create_directories(a_dir);
    fs::create_directories(b_dir);

    write_file(a_dir / "A.json",
               R"json({"id":"A","type":"release","mainClass":"example.Main"})json");
    write_file(a_dir / "A.jar", "jar");
    write_file(b_dir / "B.json",
               R"json({"id":"B","inheritsFrom":"A","jar":"A","type":"release"})json");

    // 加载器分类：Forge / NeoForge / Fabric / Quilt / OptiFine 分开识别。
    const auto make_instance = [&](const std::string& name, const std::string& body) {
        const fs::path dir = versions / name;
        fs::create_directories(dir);
        write_file(dir / (name + ".json"), body);
    };
    make_instance("F", R"json({"id":"F","libraries":[{"name":"net.minecraftforge:forge:1.20.1-47.2.0"}]})json");
    make_instance("N", R"json({"id":"N","mainClass":"cpw.mods.bootstraplauncher.BootstrapLauncher","libraries":[{"name":"net.neoforged:neoforge:21.1.0"}]})json");
    make_instance("Fa", R"json({"id":"Fa","libraries":[{"name":"net.fabricmc:fabric-loader:0.15.0"}]})json");
    make_instance("Q", R"json({"id":"Q","libraries":[{"name":"org.quiltmc:quilt-loader:0.20.0"}]})json");
    make_instance("O", R"json({"id":"O","libraries":[{"name":"optifine:OptiFine:1.20.1_HD_U_I6"}]})json");
    make_instance("OldForge", R"json({"id":"OldForge","libraries":[{"name":"net.minecraftforge:minecraftforge:9.11.1.1345"}]})json");
    make_instance("1.20.1-Forge_47", R"json({"id":"1.20.1-Forge_47"})json");

    auto scanned = cl::mc::scan_instances(root);
    CHECK(scanned.ok());
    const auto find_loader = [&](const std::string& name) -> std::string {
        const auto& list = scanned.value();
        const auto it = std::find_if(list.begin(), list.end(), [&](const auto& item) {
            return item.id == name;
        });
        return it == list.end() ? "<missing>" : it->loader;
    };
    CHECK(find_loader("A") == "vanilla");
    CHECK(find_loader("B") == "vanilla");
    CHECK(find_loader("F") == "forge");
    CHECK(find_loader("OldForge") == "forge");
    CHECK(find_loader("1.20.1-Forge_47") == "forge");
    CHECK(find_loader("N") == "neoforge");
    CHECK(find_loader("Fa") == "fabric");
    CHECK(find_loader("Q") == "quilt");
    CHECK(find_loader("O") == "optifine");

    // 实例设置读写。
    cl::mc::InstanceSettings settings;
    settings.isolate = true;
    auto saved = cl::mc::save_instance_settings(a_dir, settings);
    CHECK(saved.ok());
    auto loaded = cl::mc::load_instance_settings(a_dir);
    CHECK(loaded.ok());
    CHECK(loaded.value().isolate);

    settings.isolate = false;
    saved = cl::mc::save_instance_settings(a_dir, settings);
    CHECK(saved.ok());
    loaded = cl::mc::load_instance_settings(a_dir);
    CHECK(loaded.ok());
    CHECK(!loaded.value().isolate);

    // 重命名：目录、JSON、JAR 和子版本引用都要更新。
    auto renamed = cl::mc::rename_instance(root, "A", "C");
    CHECK(renamed.ok());
    CHECK(renamed.value() == "C");
    CHECK(!fs::exists(a_dir));
    CHECK(fs::is_directory(versions / "C"));
    CHECK(!fs::exists(versions / "C" / "A.json"));
    CHECK(fs::is_regular_file(versions / "C" / "C.json"));
    CHECK(fs::is_regular_file(versions / "C" / "C.jar"));

    const auto c_json = read_json(versions / "C" / "C.json");
    CHECK(c_json["id"] == "C");
    const auto b_json = read_json(b_dir / "B.json");
    CHECK(b_json["inheritsFrom"] == "C");
    CHECK(b_json["jar"] == "C");

    // 非法名称和目标已存在都要失败。
    auto invalid = cl::mc::rename_instance(root, "C", "bad/name");
    CHECK(!invalid.ok());
    fs::create_directories(versions / "D");
    write_file(versions / "D" / "D.json", R"json({"id":"D"})json");
    auto conflict = cl::mc::rename_instance(root, "C", "D");
    CHECK(!conflict.ok());
    auto missing = cl::mc::rename_instance(root, "Nope", "E");
    CHECK(!missing.ok());

    // 资源管理：未隔离时使用全局 mods，.disabled 视为禁用。
    fs::create_directories(root / "mods");
    write_file(root / "mods" / "alpha.jar", "a");
    write_file(root / "mods" / "beta.jar.disabled", "b");

    auto resources = cl::mc::list_instance_resources(root, "C", "mods");
    CHECK(resources.ok());
    CHECK(resources.value().dir == root / "mods");
    CHECK(resources.value().shared);
    CHECK(resources.value().entries.size() == 2);
    CHECK(resources.value().entries[0].name == "alpha.jar");
    CHECK(resources.value().entries[0].enabled);
    CHECK(resources.value().entries[0].size == 1);
    CHECK(resources.value().entries[1].name == "beta.jar.disabled");
    CHECK(!resources.value().entries[1].enabled);

    auto toggled = cl::mc::toggle_instance_resource(root, "C", "mods", "alpha.jar");
    CHECK(toggled.ok());
    CHECK(toggled.value() == "alpha.jar.disabled");
    CHECK(!fs::exists(root / "mods" / "alpha.jar"));
    CHECK(fs::exists(root / "mods" / "alpha.jar.disabled"));

    toggled = cl::mc::toggle_instance_resource(root, "C", "mods",
                                               "alpha.jar.disabled");
    CHECK(toggled.ok());
    CHECK(toggled.value() == "alpha.jar");
    CHECK(fs::exists(root / "mods" / "alpha.jar"));

    // 存档单独隔离的覆盖层建立后，资源目录切到 versions/<id>/game。
    fs::create_directories(versions / "C" / "game" / "mods");
    write_file(versions / "C" / "game" / "mods" / "gamma.jar", "c");
    resources = cl::mc::list_instance_resources(root, "C", "mods");
    CHECK(resources.ok());
    CHECK(resources.value().dir == versions / "C" / "game" / "mods");
    CHECK(!resources.value().shared);
    CHECK(resources.value().entries.size() == 1);
    CHECK(resources.value().entries[0].name == "gamma.jar");

    // 完整隔离始终使用实例自己的 game 目录。
    cl::mc::InstanceSettings isolated;
    isolated.isolate = true;
    CHECK(cl::mc::save_instance_settings(versions / "C", isolated).ok());
    resources = cl::mc::list_instance_resources(root, "C", "mods");
    CHECK(resources.ok());
    CHECK(resources.value().dir == versions / "C" / "game" / "mods");
    CHECK(!resources.value().shared);

    // 非法类型、路径穿越和缺失文件都要报错。
    auto bad_kind = cl::mc::list_instance_resources(root, "C", "config");
    CHECK(!bad_kind.ok());
    auto bad_name =
        cl::mc::toggle_instance_resource(root, "C", "mods", "../gamma.jar");
    CHECK(!bad_name.ok());
    auto missing_resource =
        cl::mc::delete_instance_resource(root, "C", "mods", "nope.jar");
    CHECK(!missing_resource.ok());

    fs::remove_all(root, ec);
    std::cout << "instance_test: all checks passed\n";
    return 0;
}
