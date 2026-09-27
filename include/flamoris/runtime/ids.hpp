#pragma once

#include "flamoris/runtime/result.hpp"
#include <charconv>
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

namespace flamoris::runtime {

[[nodiscard]] Result<std::uint64_t> parse_counter(std::string_view text) noexcept;

template<typename Tag> class StrongId final {
public:
    constexpr StrongId() noexcept = default;
    explicit constexpr StrongId(std::uint64_t value) noexcept : value_(value) {}
    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
    [[nodiscard]] static Result<StrongId> from_wire(std::string_view text) noexcept {
        auto parsed = parse_counter(text);
        if (!parsed) return Result<StrongId>::failure(parsed.error());
        if (parsed.value() == 0) return Result<StrongId>::failure(ErrorEnvelope::make(
            ErrorCode::invalid_request, ErrorStage::validation, ExternalOutcome::not_applicable,
            RetryDisposition::prohibited, ErrorReason::malformed_identifier));
        return Result<StrongId>::success(StrongId(parsed.value()));
    }
    [[nodiscard]] std::string to_wire() const { return std::to_string(value_); }
    auto operator<=>(const StrongId&) const = default;
private:
    std::uint64_t value_{};
};

#define FLAMORIS_STRONG_ID(Name) struct Name##Tag; using Name = StrongId<Name##Tag>
FLAMORIS_STRONG_ID(AttemptId);
FLAMORIS_STRONG_ID(OperationId);
FLAMORIS_STRONG_ID(AllocationId);
FLAMORIS_STRONG_ID(ReservationId);
FLAMORIS_STRONG_ID(LeaseId);
FLAMORIS_STRONG_ID(CleanupId);
FLAMORIS_STRONG_ID(CommandId);
FLAMORIS_STRONG_ID(TransitionId);
FLAMORIS_STRONG_ID(PendingSubmissionId);
FLAMORIS_STRONG_ID(StateReferenceId);
FLAMORIS_STRONG_ID(NativeWorkerGeneration);
FLAMORIS_STRONG_ID(HostEpoch);
FLAMORIS_STRONG_ID(DispatchGeneration);
FLAMORIS_STRONG_ID(ActivationGeneration);
FLAMORIS_STRONG_ID(SuspensionGeneration);
FLAMORIS_STRONG_ID(OperationGeneration);
FLAMORIS_STRONG_ID(LedgerRevision);
FLAMORIS_STRONG_ID(PolicyRevision);
#undef FLAMORIS_STRONG_ID

class RuntimeInstanceId final {
public:
    constexpr RuntimeInstanceId() noexcept = default;
    constexpr RuntimeInstanceId(std::uint64_t high, std::uint64_t low) noexcept : high_(high), low_(low) {}
    [[nodiscard]] constexpr std::uint64_t high() const noexcept { return high_; }
    [[nodiscard]] constexpr std::uint64_t low() const noexcept { return low_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return high_ != 0 || low_ != 0; }
    auto operator<=>(const RuntimeInstanceId&) const = default;
private:
    std::uint64_t high_{};
    std::uint64_t low_{};
};

class RunId final {
public:
    constexpr RunId() noexcept = default;
    constexpr RunId(RuntimeInstanceId instance, std::uint64_t value) noexcept : instance_(instance), value_(value) {}
    [[nodiscard]] constexpr RuntimeInstanceId instance() const noexcept { return instance_; }
    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return instance_.valid() && value_ != 0; }
    auto operator<=>(const RunId&) const = default;
private:
    RuntimeInstanceId instance_;
    std::uint64_t value_{};
};

class JobId final {
public:
    constexpr JobId() noexcept = default;
    constexpr JobId(RunId run, std::uint64_t value) noexcept : run_(run), value_(value) {}
    [[nodiscard]] constexpr RunId run() const noexcept { return run_; }
    [[nodiscard]] constexpr RuntimeInstanceId instance() const noexcept { return run_.instance(); }
    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return run_.valid() && value_ != 0; }
    auto operator<=>(const JobId&) const = default;
private:
    RunId run_;
    std::uint64_t value_{};
};

// Implementations are instance-owned. Production sources must provide fresh incarnation
// entropy; the deterministic implementation exists only under tests/support.
class IdSource {
public:
    virtual ~IdSource() = default;
    [[nodiscard]] virtual Result<RuntimeInstanceId> next_instance() noexcept = 0;
};

} // namespace flamoris::runtime
