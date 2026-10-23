#pragma once

#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace cl {

struct Error {
    int code = -1;
    std::string message;
    std::string context;

    static Error make(int code, std::string message, std::string context = {}) {
        return Error{code, std::move(message), std::move(context)};
    }

    [[nodiscard]] std::string to_string() const {
        if (context.empty()) return message;
        return message + " (" + context + ")";
    }
};

template <typename T>
class Result {
public:
    Result(T value) : data_(std::move(value)) {}
    Result(Error e) : data_(std::move(e)) {}

    [[nodiscard]] bool ok() const noexcept { return data_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    T& value() & { return std::get<0>(data_); }
    const T& value() const& { return std::get<0>(data_); }
    T&& value() && { return std::move(std::get<0>(data_)); }

    const Error& error() const& { return std::get<1>(data_); }

private:
    std::variant<T, Error> data_;
};

template <>
class Result<void> {
public:
    Result() = default;
    Result(Error e) : error_(std::move(e)) {}

    [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    const Error& error() const& { return *error_; }

private:
    std::optional<Error> error_;
};

} // namespace cl

// 把 Result 中的值取出来；失败时直接把错误返回给调用方。
#define CL_TRY(type, name, expr)                          \
    auto name##_result = (expr);                          \
    if (!name##_result) return name##_result.error();     \
    type name = std::move(name##_result).value()

#define CL_TRY_VOID(expr)                                 \
    do {                                                  \
        auto cl_result = (expr);                          \
        if (!cl_result) return cl_result.error();         \
    } while (0)
