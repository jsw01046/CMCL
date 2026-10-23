#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace cl::mc {

struct InstanceSummary {
    std::string id;
    std::string type;
    std::string loader;  // vanilla / forge / neoforge / fabric / quilt / optifine
    std::string main_class;
    std::optional<std::string> inherits_from;
    bool valid = true;
    bool isolate = false;
    std::string error;
};

struct InstanceSettings {
    bool isolate = false;
    // 存档单独隔离：mods、config 等仍共享，只把 saves 放到实例自己的目录。
    bool isolate_saves = true;
};

struct ResourceEntry {
    std::string name;
    bool directory = false;
    bool enabled = true;
    unsigned long long size = 0;
    long long modified = 0;  // Unix 秒
};

struct InstanceResourceList {
    std::filesystem::path dir;
    bool isolate = false;
    bool isolate_saves = true;
    bool shared = false;  // 当前目录是否与其它实例共用（全局目录或目录联接）
    std::vector<ResourceEntry> entries;
};

// 实例资源目录（mods / resourcepacks / shaderpacks / datapacks）。
// 未指定实例时返回全局目录；存档单独隔离且覆盖层尚未建立时回退到全局目录。
Result<std::filesystem::path> instance_resource_dir(
    const std::filesystem::path& mc_dir, const std::string& id,
    const std::string& kind);

// 列出资源目录，目录不存在时自动创建。
Result<InstanceResourceList> list_instance_resources(
    const std::filesystem::path& mc_dir, const std::string& id,
    const std::string& kind);

// 删除单个资源（移到回收站）。
Result<void> delete_instance_resource(const std::filesystem::path& mc_dir,
                                      const std::string& id,
                                      const std::string& kind,
                                      const std::string& name);

// 启用/禁用单个资源：在文件名后追加或移除 .disabled，返回新的文件名。
Result<std::string> toggle_instance_resource(const std::filesystem::path& mc_dir,
                                             const std::string& id,
                                             const std::string& kind,
                                             const std::string& name);

// 在资源管理器中打开资源目录。
Result<void> open_instance_resources(const std::filesystem::path& mc_dir,
                                     const std::string& id,
                                     const std::string& kind);

bool is_valid_instance_name(std::string_view name);

// 确保游戏目录存在，并补齐 versions / libraries / assets / logs 等基础子目录。
// 目录已存在时只做检查，不会清理里面的文件。
Result<void> ensure_game_dir(const std::filesystem::path& mc_dir);

// 扫描 <mc_dir>/versions 下的实例。无效项也会返回，方便 UI 标红。
Result<std::vector<InstanceSummary>> scan_instances(
    const std::filesystem::path& mc_dir);

// 每个实例自己的设置，存放在 versions/<id>/craftlauncher.json。
Result<InstanceSettings> load_instance_settings(
    const std::filesystem::path& version_dir);
Result<void> save_instance_settings(const std::filesystem::path& version_dir,
                                    const InstanceSettings& settings);

// 重命名实例目录、JSON、主 jar，并修正其它实例里对它的 inheritsFrom/jar 引用。
Result<std::string> rename_instance(const std::filesystem::path& mc_dir,
                                    const std::string& old_id,
                                    const std::string& new_id);

// 删除实例目录（移到回收站）。
Result<void> delete_instance(const std::filesystem::path& mc_dir,
                             const std::string& id);

// 计算并创建实例当前的存档目录。
Result<std::filesystem::path> instance_saves_dir(
    const std::filesystem::path& mc_dir, const std::string& id);

// 在资源管理器中打开实例目录。
Result<void> open_instance_folder(const std::filesystem::path& mc_dir,
                                  const std::string& id);

// 在资源管理器中打开实例的存档目录。
Result<void> open_instance_saves(const std::filesystem::path& mc_dir,
                                 const std::string& id);

} // namespace cl::mc
