#include "catch_amalgamated.hpp"
#include "flamoris/runtime/effects.hpp"
#include "flamoris/runtime/event.hpp"
#include "flamoris/runtime/limits.hpp"
#include "support/deterministic.hpp"
#include <array>
#include <limits>
#include <memory>
#include <type_traits>

using namespace flamoris::runtime;
using namespace std::chrono_literals;

TEST_CASE("A06 effect algebra rejects every invalid bit set and composes pure members") {
    for (unsigned bits = 0; bits < 256; ++bits) {
        const bool valid = bits != 0 && bits < 64 && ((bits & 1U) == 0 || bits == 1) &&
                           ((bits & 16U) == 0 || (bits & 4U) != 0);
        INFO(bits);
        REQUIRE(EffectSet::from_mask(static_cast<std::uint8_t>(bits)).has_value() == valid);
    }
    const auto pure = EffectSet::from_names({"pure"}).value();
    const auto write = EffectSet::from_names({"write", "paid"}).value();
    const std::array members{pure, write};
    REQUIRE(EffectSet::aggregate(members).value() == write);
    REQUIRE_FALSE(EffectSet::aggregate({}));
    REQUIRE_FALSE(EffectSet::from_names({"pure", "pure"}));
    REQUIRE_FALSE(EffectSet::from_names({"read", "future"}));
    REQUIRE(EffectSet::aggregate(std::array{pure, pure}).value() == pure);
    for (unsigned a = 1; a < 64; ++a)
        for (unsigned b = 1; b < 64; ++b) {
            const auto x = EffectSet::from_mask(static_cast<std::uint8_t>(a));
            const auto y = EffectSet::from_mask(static_cast<std::uint8_t>(b));
            if (!x || !y)
                continue;
            auto xy = EffectSet::aggregate(std::array{x.value(), y.value()});
            auto yx = EffectSet::aggregate(std::array{y.value(), x.value()});
            REQUIRE(xy);
            REQUIRE(xy.value() == yx.value());
        }
}

TEST_CASE("checked identities and counters reject overflow and malformed wire versions") {
    static_assert(!std::is_convertible_v<AttemptId, OperationId>);
    static_assert(!std::is_default_constructible_v<Result<int>>);
    static_assert(!std::is_default_constructible_v<EffectSet>);
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    CheckedCounter counter(max - 1);
    REQUIRE(counter.next().value() == max);
    REQUIRE_FALSE(counter.next());
    REQUIRE(counter.value() == max);
    REQUIRE(parse_counter("18446744073709551615").value() == max);
    for (auto bad : {"", "01", "-1", "+1", "1.0", " 1", "18446744073709551616", "v2:1"})
        REQUIRE_FALSE(parse_counter(bad));
    REQUIRE_FALSE(AttemptId::from_wire("0"));
    REQUIRE(AttemptId::from_wire("1").value() == AttemptId{1});
    REQUIRE_FALSE(checked_add(max, 1));
    REQUIRE_FALSE(checked_multiply(max, 2));
    REQUIRE(checked_multiply(max, 0).value() == 0);
    REQUIRE_FALSE(FiniteLimit::create(0));
    testing::DeterministicIdSource ids(3);
    auto first = ids.next_instance().value();
    auto next = ids.next_instance().value();
    REQUIRE(first != next);
    REQUIRE(JobId{RunId{first, 1}, 1} != JobId{RunId{next, 1}, 1});
}

TEST_CASE("Result owns move-only values and reports the checked alternative") {
    auto value = Result<std::unique_ptr<int>>::success(std::make_unique<int>(42));
    REQUIRE(*value.value() == 42);
    auto error = Result<int>::failure(ErrorEnvelope::make(ErrorCode::invalid_request));
    REQUIRE_FALSE(error);
    REQUIRE_THROWS_AS(error.value(), std::bad_variant_access);
    REQUIRE(Result<void>::success());
    REQUIRE_FALSE(Result<void>::failure(error.error()));
}

TEST_CASE("error values contain only bounded Runtime classifications") {
    const std::array causes{ErrorCode::upstream_failure, static_cast<ErrorCode>(999)};
    auto error =
        ErrorEnvelope::make(ErrorCode::job_timeout, ErrorStage::execution, ExternalOutcome::unknown,
                            RetryDisposition::policy_eligible, ErrorReason::none, causes);
    REQUIRE(error.code() == ErrorCode::job_timeout);
    REQUIRE(error.external_outcome() == ExternalOutcome::unknown);
    REQUIRE(error.retry_disposition() == RetryDisposition::reconciliation_required);
    REQUIRE(error.cause_codes()[1] == ErrorCode::internal_error);
    REQUIRE(error.message().size() <= 256);
    std::array<ErrorCode, 9> many{};
    REQUIRE(ErrorEnvelope::make(ErrorCode::upstream_failure, ErrorStage::execution,
                                ExternalOutcome::unknown, RetryDisposition::prohibited,
                                ErrorReason::none, many)
                .cause_codes()
                .size() == 8);
    REQUIRE(ErrorEnvelope::make(static_cast<ErrorCode>(1000)).code() == ErrorCode::internal_error);
}

TEST_CASE("event reservations use checked total maxima and exact storage boundary") {
    EventReservation bounds{1, 1, 1, 1, 1, 1, 1, 1, 4};
    const auto slots = mandatory_event_slots(bounds).value();
    REQUIRE(slots == 112);
    REQUIRE(mandatory_event_storage(bounds, slots * 8192).value() == slots * 8192);
    REQUIRE_FALSE(mandatory_event_storage(bounds, slots * 8192 - 1));
    bounds.jobs = std::numeric_limits<std::uint64_t>::max();
    REQUIRE_FALSE(mandatory_event_slots(bounds));
    REQUIRE_FALSE(EventValue::make(static_cast<EventKind>(1000)));
    REQUIRE_FALSE(EventValue::make(EventKind::attempt_started, {}, AttemptId{1}));
}

TEST_CASE("manual clock expiry is independent from delivery and equality expires") {
    testing::ManualClock clock;
    testing::ManualTimerQueue timers(1);
    bool delivered = false;
    auto deadline = Deadline::after(clock.now(), 1s).value();
    auto timer = timers.schedule(deadline, [&] { delivered = true; }).value();
    REQUIRE_FALSE(timers.deliver(timer, clock.now()));
    REQUIRE(clock.advance(1s));
    REQUIRE(deadline.expired(clock.now()));
    REQUIRE_FALSE(delivered);
    REQUIRE_FALSE(timers.schedule(deadline, [] {}));
    REQUIRE(timers.deliver(timer, clock.now()));
    REQUIRE(delivered);
    REQUIRE_FALSE(timers.deliver(timer, clock.now()));
    REQUIRE_FALSE(clock.advance(-1ns));
    REQUIRE_FALSE(Deadline::after(TimePoint::max(), 1ns));
}
