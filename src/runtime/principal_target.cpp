#include "flamoris/runtime/principal_target.hpp"

namespace flamoris::runtime {
namespace {
template <class T> Result<T> denied() {
    return Result<T>::failure(ErrorEnvelope::make(ErrorCode::permission_denied));
}
} // namespace
Result<std::unique_ptr<PrincipalRuntimeTarget>>
PrincipalRuntimeTarget::create(std::string subject, std::unique_ptr<RuntimeInstance> runtime) {
    if (subject.empty() || subject.size() > 128 || !runtime)
        return Result<std::unique_ptr<PrincipalRuntimeTarget>>::failure(
            ErrorEnvelope::make(ErrorCode::invalid_request));
    return Result<std::unique_ptr<PrincipalRuntimeTarget>>::success(
        std::unique_ptr<PrincipalRuntimeTarget>(
            new PrincipalRuntimeTarget(std::move(subject), std::move(runtime))));
}
RuntimeInstanceId PrincipalRuntimeTarget::instance_id() const noexcept {
    return runtime_->instance_id();
}
bool PrincipalRuntimeTarget::permits(const AuthorizationContext &context) const noexcept {
    return !context.revoked && context.subject == subject_;
}
Result<SubmissionReceipt> PrincipalRuntimeTarget::submit(const AuthorizationContext &context,
                                                         std::string_view input) {
    if (!permits(context))
        return denied<SubmissionReceipt>();
    return runtime_->submit(context, input);
}
Result<SubmissionReceipt> PrincipalRuntimeTarget::submit_pinned(const AuthorizationContext &context,
                                                                std::string_view input,
                                                                PinnedSubmissionIdentity expected) {
    if (!permits(context))
        return denied<SubmissionReceipt>();
    return runtime_->submit_pinned(context, input, std::move(expected));
}
Result<RunSnapshot> PrincipalRuntimeTarget::status(const AuthorizationContext &context, RunId run) {
    if (!permits(context))
        return denied<RunSnapshot>();
    return runtime_->status(context, run);
}
Result<RunResult> PrincipalRuntimeTarget::result(const AuthorizationContext &context, RunId run) {
    if (!permits(context))
        return denied<RunResult>();
    return runtime_->result(context, run);
}
Result<CommandReceipt> PrincipalRuntimeTarget::cancel(const AuthorizationContext &context,
                                                      RunId run) {
    if (!permits(context))
        return denied<CommandReceipt>();
    return runtime_->cancel(context, run);
}
Result<CommandReceipt> PrincipalRuntimeTarget::pause(const AuthorizationContext &context, RunId run,
                                                     std::uint64_t command) {
    if (!permits(context))
        return denied<CommandReceipt>();
    return runtime_->pause(context, run, command);
}
Result<CommandReceipt> PrincipalRuntimeTarget::resume(const AuthorizationContext &context,
                                                      RunId run, std::uint64_t command) {
    if (!permits(context))
        return denied<CommandReceipt>();
    return runtime_->resume(context, run, command);
}
Result<ObservationPage> PrincipalRuntimeTarget::events(const AuthorizationContext &context,
                                                       RunId run, std::uint64_t after,
                                                       std::size_t limit) {
    if (!permits(context))
        return denied<ObservationPage>();
    return runtime_->events(context, run, after, limit);
}
Result<ReplaySnapshot> PrincipalRuntimeTarget::replay(const AuthorizationContext &context,
                                                      RunId run) {
    if (!permits(context))
        return denied<ReplaySnapshot>();
    return runtime_->replay(context, run);
}
} // namespace flamoris::runtime
