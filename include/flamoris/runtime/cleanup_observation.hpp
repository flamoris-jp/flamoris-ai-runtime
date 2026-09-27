#pragma once
#include "flamoris/runtime/lifecycle.hpp"
#include "flamoris/runtime/resources.hpp"

namespace flamoris::runtime {
class RunObservationLookupPort {
  public:
    virtual ~RunObservationLookupPort() = default;
    // Control-executor synchronous lookup only. Never retained in a callback.
    virtual RunController *find_observation_run(RunId) noexcept = 0;
};
class ControllerCleanupObservationPort final : public CleanupObservationPort {
  public:
    ControllerCleanupObservationPort(RunObservationLookupPort &, MonotonicClock &,
                                     std::size_t max_records = 128);
    Result<CleanupObservationTicket> reserve(RunId, CleanupId, TimePoint) override;
    void observed(CleanupObservationTicket, CleanupId, LedgerRevision) noexcept override;
    void closed(CleanupObservationTicket, CleanupId, bool resolved,
                LedgerRevision) noexcept override;
    bool healthy() const noexcept { return healthy_; }

  private:
    struct Record {
        std::uint64_t ticket, reservation;
        RunId run;
        CleanupId cleanup;
        TimePoint closes_at;
        EventGroup observed, closed;
        bool observed_published{false}, closed_published{false};
    };
    RunObservationLookupPort &lookup_;
    MonotonicClock &clock_;
    std::size_t max_records_;
    std::uint64_t next_ticket_{1};
    std::vector<Record> records_;
    bool healthy_{true};
};
} // namespace flamoris::runtime
