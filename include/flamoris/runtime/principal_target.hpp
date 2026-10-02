#pragma once
#include "flamoris/runtime/runtime.hpp"

namespace flamoris::runtime {
// Trusted embedding boundary: one configured authenticated subject owns one
// independent RuntimeInstance. This does not make the kernel multi-tenant.
class PrincipalRuntimeTarget final : public RuntimeCommandPort {
  public:
    static Result<std::unique_ptr<PrincipalRuntimeTarget>> create(std::string subject,
                                                                  std::unique_ptr<RuntimeInstance>);
    PrincipalRuntimeTarget(const PrincipalRuntimeTarget &) = delete;
    PrincipalRuntimeTarget &operator=(const PrincipalRuntimeTarget &) = delete;
    RuntimeInstanceId instance_id() const noexcept;
    Result<SubmissionReceipt> submit(const AuthorizationContext &, std::string_view) override;
    Result<SubmissionReceipt> submit_pinned(const AuthorizationContext &, std::string_view,
                                            PinnedSubmissionIdentity);
    Result<RunSnapshot> status(const AuthorizationContext &, RunId) override;
    Result<RunResult> result(const AuthorizationContext &, RunId) override;
    Result<CommandReceipt> cancel(const AuthorizationContext &, RunId) override;
    Result<CommandReceipt> pause(const AuthorizationContext &, RunId, std::uint64_t) override;
    Result<CommandReceipt> resume(const AuthorizationContext &, RunId, std::uint64_t) override;
    Result<ObservationPage> events(const AuthorizationContext &, RunId, std::uint64_t,
                                   std::size_t) override;
    Result<ReplaySnapshot> replay(const AuthorizationContext &, RunId) override;
    // Administrative capability for the trusted embedding/host binding only.
    // Give ordinary transports only RuntimeCommandPort; never publish this seam.
    RuntimeInstance &trusted_runtime() noexcept { return *runtime_; }

  private:
    PrincipalRuntimeTarget(std::string subject, std::unique_ptr<RuntimeInstance> runtime)
        : subject_(std::move(subject)), runtime_(std::move(runtime)) {}
    bool permits(const AuthorizationContext &) const noexcept;
    const std::string subject_;
    const std::unique_ptr<RuntimeInstance> runtime_;
};
} // namespace flamoris::runtime
