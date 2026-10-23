#include "core/process.h"

#include <windows.h>

#include "core/encoding.h"
#include "core/log.h"

namespace cl {

namespace {

Error last_error(std::string_view what) {
    const DWORD code = ::GetLastError();
    std::wstring text;
    wchar_t* buffer = nullptr;
    const DWORD size = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    if (size != 0 && buffer != nullptr) {
        text.assign(buffer, size);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) {
            text.pop_back();
        }
    }
    if (buffer != nullptr) ::LocalFree(buffer);
    return Error::make(static_cast<int>(code), std::string(what), to_utf8(text));
}

} // namespace

std::wstring quote_arg(const std::wstring& arg) {
    if (arg.empty()) return L"\"\"";

    if (arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }

    std::wstring out;
    out.push_back(L'"');
    size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            backslashes = 0;
            out.push_back(c);
        }
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring build_command_line(const std::vector<std::wstring>& args) {
    std::wstring line;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i != 0) line.push_back(L' ');
        line += quote_arg(args[i]);
    }
    return line;
}

Result<std::unique_ptr<Process>> Process::start(
    const std::wstring& command_line,
    const std::filesystem::path& working_dir,
    OutputFn on_output,
    unsigned int output_code_page) {
    auto self = std::unique_ptr<Process>(new Process());
    self->on_output_ = std::move(on_output);
    self->output_code_page_ = output_code_page;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!::CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        return last_error("CreatePipe 失败");
    }
    if (!::SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
        const Error err = last_error("SetHandleInformation 失败");
        ::CloseHandle(read_pipe);
        ::CloseHandle(write_pipe);
        return err;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmd(command_line.begin(), command_line.end());
    cmd.push_back(L'\0');

    const std::wstring cwd = working_dir.empty() ? std::wstring{} : working_dir.wstring();

    if (!::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                          CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr,
                          cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
        const Error err = last_error("CreateProcess 失败");
        ::CloseHandle(read_pipe);
        ::CloseHandle(write_pipe);
        return err;
    }

    ::CloseHandle(pi.hThread);
    ::CloseHandle(write_pipe);

    self->process_ = pi.hProcess;
    self->pid_ = pi.dwProcessId;
    self->read_pipe_ = read_pipe;

    // Job Object：启动器退出时一并结束游戏进程树（与 PCL 的行为一致）。
    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info,
                                      sizeof(info)) &&
            ::AssignProcessToJobObject(job, pi.hProcess)) {
            self->job_ = job;
        } else {
            Log::warn("无法把游戏进程加入 Job Object，关闭启动器时可能残留 java 进程");
            ::CloseHandle(job);
        }
    }

    self->reader_ = std::thread([raw = self.get()] { raw->read_loop(); });
    return self;
}

Process::~Process() {
    if (running()) {
        terminate();
    }
    if (reader_.joinable()) {
        reader_.join();
    }
    if (read_pipe_ != nullptr) ::CloseHandle(static_cast<HANDLE>(read_pipe_));
    if (job_ != nullptr) ::CloseHandle(static_cast<HANDLE>(job_));
    if (process_ != nullptr) ::CloseHandle(static_cast<HANDLE>(process_));
}

bool Process::running() const {
    if (process_ == nullptr) return false;
    return ::WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT;
}

unsigned long Process::pid() const { return pid_; }

unsigned long Process::wait() {
    if (process_ == nullptr) return 0;
    ::WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
    if (reader_.joinable()) {
        reader_.join();
    }
    DWORD code = 0;
    ::GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
    return code;
}

void Process::terminate() {
    if (process_ != nullptr) {
        ::TerminateProcess(static_cast<HANDLE>(process_), 1);
    }
}

void Process::read_loop() {
    char buffer[4096];
    std::string pending;
    DWORD read = 0;
    while (::ReadFile(static_cast<HANDLE>(read_pipe_), buffer, sizeof(buffer), &read, nullptr) &&
           read > 0) {
        pending.append(buffer, read);
        size_t pos = 0;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::string decoded = decode_to_utf8(line, output_code_page_);
            if (on_output_) on_output_(decoded);
            pending.erase(0, pos + 1);
        }
    }
    if (!pending.empty() && on_output_) {
        const std::string decoded = decode_to_utf8(pending, output_code_page_);
        on_output_(decoded);
    }
}

} // namespace cl
