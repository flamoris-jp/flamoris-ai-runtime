#pragma once

#include "flamoris/runtime/error.hpp"
#include <type_traits>
#include <utility>
#include <variant>

namespace flamoris::runtime {

// No default-success value. A wrong-alternative access throws std::bad_variant_access
// rather than producing undefined behavior; source API boundaries must catch exceptions.
template <typename T> class [[nodiscard]] Result final {
  public:
    [[nodiscard]] static Result success(T value) noexcept(std::is_nothrow_move_constructible_v<T>) {
        return Result(std::in_place_index<0>, std::move(value));
    }
    [[nodiscard]] static Result failure(ErrorEnvelope error) noexcept {
        return Result(std::in_place_index<1>, std::move(error));
    }
    [[nodiscard]] bool has_value() const noexcept { return data_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }
    T &value() & { return std::get<0>(data_); }
    const T &value() const & { return std::get<0>(data_); }
    T &&value() && { return std::get<0>(std::move(data_)); }
    const ErrorEnvelope &error() const & { return std::get<1>(data_); }
    ErrorEnvelope &&error() && { return std::get<1>(std::move(data_)); }

  private:
    template <std::size_t I, typename V>
    Result(std::in_place_index_t<I> index, V &&value) : data_(index, std::forward<V>(value)) {}
    std::variant<T, ErrorEnvelope> data_;
};

template <> class [[nodiscard]] Result<void> final {
  public:
    [[nodiscard]] static Result success() noexcept { return Result(std::monostate{}); }
    [[nodiscard]] static Result failure(ErrorEnvelope error) noexcept {
        return Result(std::move(error));
    }
    [[nodiscard]] bool has_value() const noexcept { return data_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }
    void value() const { (void)std::get<0>(data_); }
    const ErrorEnvelope &error() const & { return std::get<1>(data_); }
    ErrorEnvelope &&error() && { return std::get<1>(std::move(data_)); }

  private:
    explicit Result(std::monostate value) noexcept : data_(value) {}
    explicit Result(ErrorEnvelope error) noexcept : data_(std::move(error)) {}
    std::variant<std::monostate, ErrorEnvelope> data_;
};

} // namespace flamoris::runtime
