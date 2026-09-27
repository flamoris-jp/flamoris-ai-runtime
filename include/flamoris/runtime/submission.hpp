#pragma once
#include "flamoris/runtime/compiler.hpp"
#include "flamoris/runtime/ids.hpp"
#include <tuple>
namespace flamoris::runtime {
struct SubmissionTicket {
    PendingSubmissionId pending;
    RunId run;
    std::uint64_t receipt{0}, generation{0};
    bool owner{false};
};
struct SubmissionDecision {
    std::optional<RunId> run;
    std::optional<ErrorEnvelope> rejection;
};
struct SubmissionBounds {
    std::size_t max_pending{128}, max_claims{1024}, max_waiters_per_claim{32}, max_receipts{2048};
    std::uint64_t pending_timeout_ms{30000}, retention_ms{60000};
};
// Single control-executor ownership; transport obtains tickets and immutable decisions.
class SubmissionIndex {
  public:
    explicit SubmissionIndex(RuntimeInstanceId instance, SubmissionBounds bounds = {})
        : instance_(instance), bounds_(bounds) {}
    Result<SubmissionTicket> claim(std::string subject, std::string kind,
                                   std::optional<std::string> key, std::string digest,
                                   std::uint64_t now_ms);
    Result<void> admit(const SubmissionTicket &, std::uint64_t now_ms);
    Result<void> reject(const SubmissionTicket &, ErrorEnvelope);
    Result<std::shared_ptr<const SubmissionDecision>> poll(const SubmissionTicket &) const;
    void release_receipt(const SubmissionTicket &);
    void expire(std::uint64_t now_ms);
    std::size_t keyed_entries() const noexcept { return claims_.size(); }
    std::uint64_t keyed_lookups() const noexcept { return keyed_lookups_; }
    std::size_t pending_entries() const noexcept { return pending_.size(); }

  private:
    using Key = std::tuple<std::string, std::string, std::string>;
    struct Pending {
        SubmissionTicket ticket;
        std::optional<Key> key;
        std::vector<std::uint64_t> receipts;
        std::uint64_t deadline;
    };
    struct Claim {
        std::string digest;
        SubmissionTicket ticket;
        std::shared_ptr<const SubmissionDecision> decision;
        std::uint64_t expires{0};
    };
    struct Receipt {
        SubmissionTicket ticket;
        std::shared_ptr<const SubmissionDecision> decision;
    };
    Result<void> complete(const SubmissionTicket &, std::shared_ptr<const SubmissionDecision>,
                          std::uint64_t expiry);
    RuntimeInstanceId instance_;
    SubmissionBounds bounds_;
    std::uint64_t next_pending_{1}, next_run_{1}, next_receipt_{1}, keyed_lookups_{0};
    std::map<Key, Claim> claims_;
    std::map<PendingSubmissionId, Pending> pending_;
    std::map<std::uint64_t, Receipt> receipts_;
};
} // namespace flamoris::runtime
