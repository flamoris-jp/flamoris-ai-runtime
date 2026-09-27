#pragma once
#include "flamoris/runtime/authorization.hpp"
#include "flamoris/runtime/lifecycle.hpp"
#include "flamoris/runtime/observation.hpp"
#include <iosfwd>

namespace flamoris::runtime {
struct SubmissionReceipt {
    RunId id;
    bool duplicate{false};
};
struct RunResult {
    bool pending{true};
    std::optional<JsonValue> value;
    std::optional<ErrorEnvelope> error;
    ExternalOutcome external_outcome{ExternalOutcome::not_applicable};
};
struct CommandReceipt {
    bool accepted{false}, applied{false}, terminal{false};
};

// The composition root implements this port using its existing authorities.
// Authentication identifies the caller; every method still performs current
// authorization at its own boundary. Transport timeout/disconnect calls nothing.
class RuntimeCommandPort {
  public:
    virtual ~RuntimeCommandPort() = default;
    virtual Result<SubmissionReceipt> submit(const AuthorizationContext &, std::string_view) = 0;
    virtual Result<RunSnapshot> status(const AuthorizationContext &, RunId) = 0;
    virtual Result<RunResult> result(const AuthorizationContext &, RunId) = 0;
    virtual Result<CommandReceipt> cancel(const AuthorizationContext &, RunId) = 0;
    virtual Result<CommandReceipt> pause(const AuthorizationContext &, RunId,
                                         std::uint64_t command) = 0;
    virtual Result<CommandReceipt> resume(const AuthorizationContext &, RunId,
                                          std::uint64_t command) = 0;
    virtual Result<ObservationPage> events(const AuthorizationContext &, RunId, std::uint64_t after,
                                           std::size_t limit) = 0;
    virtual Result<ReplaySnapshot> replay(const AuthorizationContext &, RunId) = 0;
};

class CallerFacade {
  public:
    explicit CallerFacade(RuntimeCommandPort &runtime) : runtime_(runtime) {}
    // Closed, bounded flamoris.control/1 request -> flamoris.response/1 response.
    // The authenticated caller comes from the binding, never request JSON.
    Result<JsonValue> request(const AuthorizationContext &, std::string_view json);

  private:
    RuntimeCommandPort &runtime_;
};

std::string run_id_wire(RunId);
Result<RunId> parse_run_id(std::string_view);
JsonValue error_projection(const ErrorEnvelope &);
JsonValue observation_projection(const ObservationPage &);
// A genuine bounded stdin/stdout projection. No networking, endpoint selection,
// lifecycle ownership, or implicit cancellation is hidden in this loop.
int serve_json_lines(std::istream &, std::ostream &, CallerFacade &, const AuthorizationContext &,
                     std::size_t max_line_bytes = 1048576);
} // namespace flamoris::runtime
