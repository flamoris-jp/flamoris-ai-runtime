#pragma once
#include "flamoris/runtime/authorization.hpp"
#include "flamoris/runtime/lifecycle.hpp"
#include <string_view>

namespace flamoris::runtime {
enum class AdapterPurpose { invoke, reconcile };

class AdapterGrant {
  public:
    AdapterGrant(AdapterGrant &&) noexcept = default;
    AdapterGrant &operator=(AdapterGrant &&) noexcept = default;
    AdapterGrant(const AdapterGrant &) = delete;
    AdapterGrant &operator=(const AdapterGrant &) = delete;

  private:
    friend class AdapterAuthority;
    friend class RegisteredCapabilityAdapter;
    AdapterGrant(DispatchTicket ticket, CapabilityPin pin, std::string digest, std::string key,
                 std::uint64_t expiry, std::uint64_t revision, AdapterPurpose purpose,
                 std::string subject, bool retry_authorized)
        : ticket_(ticket), pin_(std::move(pin)), input_digest_(std::move(digest)),
          operation_key_(std::move(key)), subject_(std::move(subject)), expires_at_ms_(expiry),
          policy_revision_(revision), purpose_(purpose), retry_authorized_(retry_authorized) {}
    DispatchTicket ticket_;
    CapabilityPin pin_;
    std::string input_digest_, operation_key_, subject_;
    std::uint64_t expires_at_ms_, policy_revision_;
    AdapterPurpose purpose_;
    bool retry_authorized_;
};
class AdapterAuthority {
  public:
    // Root calls after its serialized eligibility/dispatch turn. A plan, effect
    // label, transport request ID, or portable JSON cannot construct this grant.
    Result<AdapterGrant> authorize(const AuthorizationContext &, const PolicySnapshot &,
                                   const ExecutionPlan &, const CapabilitySnapshot &,
                                   const AuthorizationRequest &, DispatchTicket,
                                   const CapabilityPin &, const JsonValue &input,
                                   std::string operation_key,
                                   AdapterPurpose purpose = AdapterPurpose::invoke,
                                   const RetryEvidence *retry = nullptr) const;
};
struct AdapterRequest {
    AdapterGrant grant;
    JsonValue input;
    std::size_t max_output_bytes{1048576};
};
struct AdapterOutcome {
    DispatchTicket ticket;
    ExternalOutcome external_outcome{ExternalOutcome::not_dispatched};
    std::optional<JsonValue> value;
    std::optional<ErrorEnvelope> error;
    bool handoff_attempted{false};
    std::optional<RetryEvidence> retry_evidence;
};

struct ProviderRetryEvidence {
    bool transient{false}, previous_stopped{false}, confirmed_no_effect{false};
};

class BoundedProviderSink {
  public:
    explicit BoundedProviderSink(std::size_t limit);
    Result<void> append(std::string_view chunk);
    void record_outcome(ExternalOutcome outcome) noexcept;
    void record_retry_evidence(ProviderRetryEvidence evidence) noexcept { retry_ = evidence; }
    ProviderRetryEvidence retry_evidence() const noexcept { return retry_; }
    ExternalOutcome outcome() const noexcept { return outcome_; }
    const std::string &bytes() const noexcept { return bytes_; }
    std::optional<ErrorEnvelope> error() const noexcept { return error_; }

  private:
    std::size_t limit_;
    std::string bytes_;
    std::optional<ErrorEnvelope> error_;
    ExternalOutcome outcome_{ExternalOutcome::unknown};
    ProviderRetryEvidence retry_;
};

// Configuration binds this port to one registered service. No endpoint, raw
// credential, arbitrary command or service URL appears in invocation input.
class RegisteredProviderPort {
  public:
    virtual ~RegisteredProviderPort() = default;
    virtual ExternalOutcome invoke(std::string_view operation_key, const JsonValue &input,
                                   std::uint64_t deadline_ms, BoundedProviderSink &) = 0;
    virtual ExternalOutcome reconcile(std::string_view operation_key,
                                      std::uint64_t deadline_ms) = 0;
};
class RegisteredCapabilityAdapter {
  public:
    RegisteredCapabilityAdapter(CapabilityContract, RegisteredProviderPort &,
                                std::size_t max_operations = 1024,
                                std::size_t max_reconciliation_queries = 4);
    AdapterOutcome invoke(AdapterRequest, std::uint64_t now_ms) noexcept;
    AdapterOutcome reconcile(AdapterGrant, std::uint64_t now_ms) noexcept;
    bool retire_before(RuntimeInstanceId, std::uint64_t minimum_live_run) noexcept;
    std::size_t dispatches() const noexcept { return dispatches_; }
    std::size_t reconciliation_queries() const noexcept { return reconciliation_queries_; }

  private:
    CapabilityContract contract_;
    CapabilityPin pin_;
    RegisteredProviderPort &provider_;
    std::size_t max_operations_, max_reconciliation_queries_, dispatches_{0},
        reconciliation_queries_{0};
    struct Invocation {
        DispatchTicket ticket;
        std::string key, digest, subject;
        ExternalOutcome outcome{ExternalOutcome::unknown};
    };
    std::vector<Invocation> consumed_;
    std::vector<DispatchTicket> reconciled_;
    std::optional<RuntimeInstanceId> instance_;
    std::uint64_t retired_before_{0};
};

// Baseline registered pure capability; exact output is its bounded declared input.
class IdentityProvider final : public RegisteredProviderPort {
  public:
    ExternalOutcome invoke(std::string_view, const JsonValue &, std::uint64_t,
                           BoundedProviderSink &) override;
    ExternalOutcome reconcile(std::string_view, std::uint64_t) override;
};
} // namespace flamoris::runtime
