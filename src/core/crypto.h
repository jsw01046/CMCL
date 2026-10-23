#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "core/result.h"

namespace cl {

// 与 Java 的 UUID.nameUUIDFromBytes(("OfflinePlayer:" + name).getBytes(UTF_8)) 一致。
Result<std::string> offline_uuid(std::string_view player_name);

// 小写十六进制 SHA-1，用于校验 Minecraft 下载文件。
Result<std::string> sha1_hex(std::string_view data);
Result<std::string> sha1_file(const std::filesystem::path& path);

// SHA-256，用于校验 OpenJDK 安装包。
Result<std::string> sha256_hex(std::string_view data);
Result<std::string> sha256_file(const std::filesystem::path& path);

// SHA-512，用于校验 Modrinth 整合包文件。
Result<std::string> sha512_hex(std::string_view data);
Result<std::string> sha512_file(const std::filesystem::path& path);

} // namespace cl
