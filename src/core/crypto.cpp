#include "core/crypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <string_view>
#include <vector>

#include "core/encoding.h"

namespace cl {

namespace {

std::string digest_to_hex(const unsigned char* digest, DWORD length) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(static_cast<size_t>(length) * 2);
    for (DWORD i = 0; i < length; ++i) {
        result.push_back(kHex[digest[i] >> 4]);
        result.push_back(kHex[digest[i] & 0x0F]);
    }
    return result;
}

Result<std::string> hash_bytes(std::string_view data, LPCWSTR algorithm,
                               const char* name, DWORD expected_length,
                               int error_base) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    NTSTATUS status =
        ::BCryptOpenAlgorithmProvider(&alg, algorithm, nullptr, 0);
    if (status < 0 || alg == nullptr) {
        return Error::make(error_base, std::string("无法打开 ") + name + " 算法");
    }

    DWORD hash_length = 0;
    DWORD written = 0;
    status = ::BCryptGetProperty(alg, BCRYPT_HASH_LENGTH,
                                 reinterpret_cast<PUCHAR>(&hash_length),
                                 sizeof(hash_length), &written, 0);
    if (status < 0 || hash_length != expected_length) {
        ::BCryptCloseAlgorithmProvider(alg, 0);
        return Error::make(error_base + 1,
                           std::string("无法获取 ") + name + " 长度");
    }

    std::array<unsigned char, 64> digest{};
    status = ::BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    if (status < 0) {
        ::BCryptCloseAlgorithmProvider(alg, 0);
        return Error::make(error_base + 2,
                           std::string("无法创建 ") + name + " 哈希对象");
    }

    status = ::BCryptHashData(
        hash,
        reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
        static_cast<ULONG>(data.size()), 0);
    if (status >= 0) {
        status = ::BCryptFinishHash(hash, digest.data(), hash_length, 0);
    }

    ::BCryptDestroyHash(hash);
    ::BCryptCloseAlgorithmProvider(alg, 0);

    if (status < 0) {
        return Error::make(error_base + 3, std::string("计算 ") + name + " 失败");
    }
    return digest_to_hex(digest.data(), hash_length);
}

Result<std::string> hash_file(const std::filesystem::path& path,
                              LPCWSTR algorithm, const char* name,
                              DWORD expected_length, int error_base) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return Error::make(error_base, "无法打开待校验文件", path_to_utf8(path));
    }

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    NTSTATUS status =
        ::BCryptOpenAlgorithmProvider(&alg, algorithm, nullptr, 0);
    if (status < 0 || alg == nullptr) {
        return Error::make(error_base + 1,
                           std::string("无法打开 ") + name + " 算法");
    }

    DWORD hash_length = 0;
    DWORD written = 0;
    status = ::BCryptGetProperty(alg, BCRYPT_HASH_LENGTH,
                                 reinterpret_cast<PUCHAR>(&hash_length),
                                 sizeof(hash_length), &written, 0);
    if (status < 0 || hash_length != expected_length) {
        ::BCryptCloseAlgorithmProvider(alg, 0);
        return Error::make(error_base + 2,
                           std::string("无法获取 ") + name + " 长度");
    }

    std::array<unsigned char, 64> digest{};
    status = ::BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    if (status < 0) {
        ::BCryptCloseAlgorithmProvider(alg, 0);
        return Error::make(error_base + 3,
                           std::string("无法创建 ") + name + " 哈希对象");
    }

    // 注意：这个函数会在下载线程里调用，Windows 默认线程栈只有 1 MB，
    // 不要把 1 MB 的缓冲区放在栈上，否则会直接栈溢出。
    std::array<char, 64 * 1024> buffer{};
    while (status >= 0 && file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize read = file.gcount();
        if (read > 0) {
            status = ::BCryptHashData(
                hash, reinterpret_cast<PUCHAR>(buffer.data()),
                static_cast<ULONG>(read), 0);
        }
    }
    if (status >= 0) {
        status = ::BCryptFinishHash(hash, digest.data(), hash_length, 0);
    }

    ::BCryptDestroyHash(hash);
    ::BCryptCloseAlgorithmProvider(alg, 0);

    if (!file.eof() && file.fail()) {
        return Error::make(error_base + 4, "读取文件失败", path_to_utf8(path));
    }
    if (status < 0) {
        return Error::make(error_base + 5,
                           std::string("计算 ") + name + " 失败",
                           path_to_utf8(path));
    }
    return digest_to_hex(digest.data(), hash_length);
}

} // namespace

Result<std::string> offline_uuid(std::string_view player_name) {
    const std::string data = "OfflinePlayer:" + std::string(player_name);

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    NTSTATUS status = ::BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0);
    if (status < 0 || alg == nullptr) {
        return Error::make(1, "无法打开 MD5 算法");
    }

    DWORD hash_length = 0;
    DWORD written = 0;
    status = ::BCryptGetProperty(alg, BCRYPT_HASH_LENGTH,
                                 reinterpret_cast<PUCHAR>(&hash_length),
                                 sizeof(hash_length), &written, 0);
    if (status < 0 || hash_length != 16) {
        ::BCryptCloseAlgorithmProvider(alg, 0);
        return Error::make(2, "无法获取 MD5 长度");
    }

    std::array<unsigned char, 16> digest{};
    status = ::BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    if (status < 0) {
        ::BCryptCloseAlgorithmProvider(alg, 0);
        return Error::make(3, "无法创建 MD5 哈希对象");
    }

    status = ::BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                              static_cast<ULONG>(data.size()), 0);
    if (status >= 0) {
        status = ::BCryptFinishHash(hash, digest.data(), hash_length, 0);
    }

    ::BCryptDestroyHash(hash);
    ::BCryptCloseAlgorithmProvider(alg, 0);

    if (status < 0) {
        return Error::make(4, "计算离线 UUID 失败");
    }

    // 与 Java UUID.nameUUIDFromBytes 相同的版本位与变体位。
    digest[6] = static_cast<unsigned char>((digest[6] & 0x0F) | 0x30);
    digest[8] = static_cast<unsigned char>((digest[8] & 0x3F) | 0x80);

    char out[37];
    std::snprintf(out, sizeof(out),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                  "%02x%02x%02x%02x%02x%02x",
                  digest[0], digest[1], digest[2], digest[3],
                  digest[4], digest[5], digest[6], digest[7],
                  digest[8], digest[9], digest[10], digest[11],
                  digest[12], digest[13], digest[14], digest[15]);
    return std::string(out);
}

Result<std::string> sha1_hex(std::string_view data) {
    return hash_bytes(data, BCRYPT_SHA1_ALGORITHM, "SHA-1", 20, 10);
}

Result<std::string> sha1_file(const std::filesystem::path& path) {
    return hash_file(path, BCRYPT_SHA1_ALGORITHM, "SHA-1", 20, 10);
}

Result<std::string> sha256_hex(std::string_view data) {
    return hash_bytes(data, BCRYPT_SHA256_ALGORITHM, "SHA-256", 32, 30);
}

Result<std::string> sha256_file(const std::filesystem::path& path) {
    return hash_file(path, BCRYPT_SHA256_ALGORITHM, "SHA-256", 32, 30);
}

Result<std::string> sha512_hex(std::string_view data) {
    return hash_bytes(data, BCRYPT_SHA512_ALGORITHM, "SHA-512", 64, 40);
}

Result<std::string> sha512_file(const std::filesystem::path& path) {
    return hash_file(path, BCRYPT_SHA512_ALGORITHM, "SHA-512", 64, 40);
}

} // namespace cl
