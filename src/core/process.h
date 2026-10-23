#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/result.h"

namespace cl {

// 按 Windows 命令行规则为单个参数加引号。
std::wstring quote_arg(const std::wstring& arg);

// 把参数列表拼成 CreateProcessW 可用的命令行。
std::wstring build_command_line(const std::vector<std::wstring>& args);

class Process {
public:
    using OutputFn = std::function<void(std::string_view)>;

    // output_code_page 用于把子进程输出解码成 UTF-8（Java 7/8 中文日志是 GBK）。
    static Result<std::unique_ptr<Process>> start(const std::wstring& command_line,
                                                  const std::filesystem::path& working_dir,
                                                  OutputFn on_output,
                                                  unsigned int output_code_page = 65001);

    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    [[nodiscard]] bool running() const;
    [[nodiscard]] unsigned long pid() const;
    // 等待进程退出，返回退出码。读线程会在这里被回收。
    unsigned long wait();
    void terminate();

private:
    Process() = default;
    void read_loop();

    void* process_ = nullptr;
    void* job_ = nullptr;
    void* read_pipe_ = nullptr;
    void* write_pipe_ = nullptr;
    std::thread reader_;
    OutputFn on_output_;
    unsigned int output_code_page_ = 65001;
    unsigned long pid_ = 0;
};

} // namespace cl
