#include "flamoris/runtime/submission.hpp"
#include <algorithm>
#include <limits>
namespace flamoris::runtime {
namespace {
ErrorEnvelope rejected(ErrorCode c = ErrorCode::invalid_request) {
    return ErrorEnvelope::make(c, ErrorStage::admission);
}
bool addition_fits(std::uint64_t a, std::uint64_t b) {
    return b <= std::numeric_limits<std::uint64_t>::max() - a;
}
} // namespace
Result<SubmissionTicket> SubmissionIndex::claim(std::string subject, std::string kind,
                                                std::optional<std::string> key, std::string digest,
                                                std::uint64_t now) {
    try {
        if (!instance_.valid() || subject.empty() || subject.size() > 128 ||
            (kind != "workflow" && kind != "inference") ||
            (digest.size() != 64 ||
             digest.find_first_not_of("0123456789abcdef") != std::string::npos) ||
            (key && (key->empty() || key->size() > 256)))
            return Result<SubmissionTicket>::failure(rejected());
        if (key) {
            JsonValue::Object check{{"key", *key}};
            if (!canonical_json(check))
                return Result<SubmissionTicket>::failure(rejected());
        }
        if (key) expire(now);
        if (receipts_.size() >= bounds_.max_receipts ||
            next_receipt_ == std::numeric_limits<std::uint64_t>::max())
            return Result<SubmissionTicket>::failure(rejected(ErrorCode::resource_unavailable));
        std::optional<Key> scoped;
        if (key) {
            scoped = Key{subject, kind, *key};
            if (keyed_lookups_ != std::numeric_limits<std::uint64_t>::max())
                ++keyed_lookups_;
            auto it = claims_.find(*scoped);
            if (it != claims_.end()) {
                if (it->second.digest != digest)
                    return Result<SubmissionTicket>::failure(rejected());
                auto ticket = it->second.ticket;
                ticket.owner = false;
                ticket.receipt = next_receipt_;
                if (!it->second.decision) {
                    auto &p = pending_.at(ticket.pending);
                    if (p.receipts.size() >= bounds_.max_waiters_per_claim + 1)
                        return Result<SubmissionTicket>::failure(
                            rejected(ErrorCode::resource_unavailable));
                    p.receipts.reserve(p.receipts.size() + 1);
                    receipts_.emplace(ticket.receipt, Receipt{ticket, {}});
                    p.receipts.push_back(ticket.receipt);
                } else
                    receipts_.emplace(ticket.receipt, Receipt{ticket, it->second.decision});
                ++next_receipt_;
                return Result<SubmissionTicket>::success(ticket);
            }
        }
        if (pending_.size() >= bounds_.max_pending ||
            (scoped && claims_.size() >= bounds_.max_claims) ||
            next_pending_ == std::numeric_limits<std::uint64_t>::max() ||
            next_run_ == std::numeric_limits<std::uint64_t>::max() ||
            !addition_fits(now, bounds_.pending_timeout_ms))
            return Result<SubmissionTicket>::failure(rejected(ErrorCode::resource_unavailable));
        SubmissionTicket ticket{PendingSubmissionId(next_pending_), RunId(instance_, next_run_),
                                next_receipt_, next_pending_, true};
        // Prepare map nodes separately, then insert nonthrowing node handles as one turn.
        std::map<PendingSubmissionId, Pending> p;
        p.emplace(ticket.pending,
                  Pending{ticket, scoped, {ticket.receipt}, now + bounds_.pending_timeout_ms});
        std::map<std::uint64_t, Receipt> r;
        r.emplace(ticket.receipt, Receipt{ticket, {}});
        std::map<Key, Claim> c;
        if (scoped)
            c.emplace(*scoped, Claim{digest, ticket, {}, 0});
        pending_.insert(p.extract(p.begin()));
        receipts_.insert(r.extract(r.begin()));
        if (scoped)
            claims_.insert(c.extract(c.begin()));
        ++next_pending_;
        ++next_run_;
        ++next_receipt_;
        return Result<SubmissionTicket>::success(ticket);
    } catch (...) {
        return Result<SubmissionTicket>::failure(rejected(ErrorCode::internal_error));
    }
}
Result<void> SubmissionIndex::complete(const SubmissionTicket &t,
                                       std::shared_ptr<const SubmissionDecision> decision,
                                       std::uint64_t expiry) {
    auto p = pending_.find(t.pending);
    if (!t.owner || p == pending_.end() || p->second.ticket.run != t.run ||
        p->second.ticket.generation != t.generation || p->second.ticket.receipt != t.receipt)
        return Result<void>::failure(rejected());
    for (auto id : p->second.receipts) {
        auto it = receipts_.find(id);
        if (it != receipts_.end())
            it->second.decision = decision;
    }
    if (p->second.key) {
        auto c = claims_.find(*p->second.key);
        if (c == claims_.end())
            return Result<void>::failure(rejected(ErrorCode::invariant_violation));
        if (decision->run) {
            c->second.decision = decision;
            c->second.expires = expiry;
        } else
            claims_.erase(c);
    }
    pending_.erase(p);
    return Result<void>::success();
}
Result<void> SubmissionIndex::admit(const SubmissionTicket &t, std::uint64_t now) {
    try {
        if (!addition_fits(now, bounds_.retention_ms))
            return Result<void>::failure(rejected());
        auto p = pending_.find(t.pending);
        if (p == pending_.end() || now >= p->second.deadline)
            return Result<void>::failure(rejected(ErrorCode::run_timeout));
        return complete(t,
                        std::make_shared<const SubmissionDecision>(SubmissionDecision{t.run, {}}),
                        now + bounds_.retention_ms);
    } catch (...) {
        return Result<void>::failure(rejected(ErrorCode::internal_error));
    }
}
Result<void> SubmissionIndex::reject(const SubmissionTicket &t, ErrorEnvelope error) {
    try {
        return complete(
            t, std::make_shared<const SubmissionDecision>(SubmissionDecision{{}, error}), 0);
    } catch (...) {
        return Result<void>::failure(rejected(ErrorCode::internal_error));
    }
}
Result<std::shared_ptr<const SubmissionDecision>>
SubmissionIndex::poll(const SubmissionTicket &t) const {
    auto it = receipts_.find(t.receipt);
    if (it == receipts_.end() || it->second.ticket.pending != t.pending ||
        it->second.ticket.run != t.run || it->second.ticket.generation != t.generation)
        return Result<std::shared_ptr<const SubmissionDecision>>::failure(rejected());
    return Result<std::shared_ptr<const SubmissionDecision>>::success(it->second.decision);
}
void SubmissionIndex::release_receipt(const SubmissionTicket &t) {
    auto r = receipts_.find(t.receipt);
    if (r == receipts_.end() || r->second.ticket.run != t.run ||
        r->second.ticket.pending != t.pending)
        return;
    auto p = pending_.find(t.pending);
    if (p != pending_.end()) {
        auto &v = p->second.receipts;
        v.erase(std::remove(v.begin(), v.end(), t.receipt), v.end());
    }
    receipts_.erase(r);
}
void SubmissionIndex::expire(std::uint64_t now) {
    for (auto it = claims_.begin(); it != claims_.end();)
        if (it->second.decision && now >= it->second.expires)
            it = claims_.erase(it);
        else
            ++it;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (now >= it->second.deadline) {
            auto ticket = it->second.ticket;
            ++it;
            (void)reject(ticket, rejected(ErrorCode::run_timeout));
        } else
            ++it;
    }
}
} // namespace flamoris::runtime
