#pragma once

#include "flamoris/runtime/result.hpp"
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

namespace flamoris::runtime {

enum class Effect : std::uint8_t {
    pure = 1,
    read = 2,
    write = 4,
    external = 8,
    destructive = 16,
    paid = 32
};
class EffectSet final {
  public:
    [[nodiscard]] static Result<EffectSet> from_mask(std::uint8_t mask) noexcept;
    [[nodiscard]] static Result<EffectSet>
    from_names(std::span<const std::string_view> names) noexcept;
    [[nodiscard]] static Result<EffectSet>
    from_names(std::initializer_list<std::string_view> names) noexcept {
        return from_names(std::span<const std::string_view>(names.begin(), names.size()));
    }
    [[nodiscard]] static Result<EffectSet> aggregate(std::span<const EffectSet> members) noexcept;
    [[nodiscard]] bool contains(Effect effect) const noexcept {
        return (mask_ & static_cast<std::uint8_t>(effect)) != 0;
    }
    [[nodiscard]] std::uint8_t mask() const noexcept { return mask_; }
    [[nodiscard]] std::vector<std::string_view> names() const;
    bool operator==(const EffectSet &) const = default;

  private:
    explicit EffectSet(std::uint8_t mask) noexcept : mask_(mask) {}
    std::uint8_t mask_;
};

} // namespace flamoris::runtime
