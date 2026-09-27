#include "catch_amalgamated.hpp"
#include "flamoris/runtime/facade.hpp"
#include <sstream>
#include <stdexcept>
using namespace flamoris::runtime;
namespace {
// This is the immediate command-port seam for projection tests only. Kernel and
// authorization acceptance remain production Runtime integration obligations.
class MappingPort final : public RuntimeCommandPort {
  public:
    RunId id{RuntimeInstanceId{UINT64_MAX, 2}, UINT64_MAX};
    unsigned submits{0}, statuses{0}, cancels{0}, event_reads{0};
    bool throw_private{false};
    std::string submitted;
    Result<SubmissionReceipt> submit(const AuthorizationContext &,
                                     std::string_view request) override {
        ++submits;
        submitted = request;
        return Result<SubmissionReceipt>::success({id, false});
    }
    Result<RunSnapshot> status(const AuthorizationContext &, RunId run) override {
        ++statuses;
        if (throw_private)
            throw std::runtime_error("private-credential-host-path");
        RunSnapshot result;
        result.id = run;
        result.root = JobId{run, 1};
        return Result<RunSnapshot>::success(std::move(result));
    }
    Result<RunResult> result(const AuthorizationContext &, RunId) override {
        return Result<RunResult>::success({});
    }
    Result<CommandReceipt> cancel(const AuthorizationContext &, RunId) override {
        ++cancels;
        return Result<CommandReceipt>::success({true, false, false});
    }
    Result<CommandReceipt> pause(const AuthorizationContext &, RunId, std::uint64_t) override {
        return Result<CommandReceipt>::success({true, false, false});
    }
    Result<CommandReceipt> resume(const AuthorizationContext &, RunId, std::uint64_t) override {
        return Result<CommandReceipt>::success({true, true, false});
    }
    Result<ObservationPage> events(const AuthorizationContext &, RunId, std::uint64_t after,
                                   std::size_t) override {
        ++event_reads;
        ObservationPage page;
        page.watermark = after;
        return Result<ObservationPage>::success(page);
    }
    Result<ReplaySnapshot> replay(const AuthorizationContext &, RunId) override {
        return Result<ReplaySnapshot>::success({});
    }
};
AuthorizationContext caller() {
    AuthorizationContext value;
    value.subject = "local";
    return value;
}
std::string request(const MappingPort &port, std::string method = "run.status") {
    return "{\"schema_version\":\"flamoris.control/1\",\"method\":\"" + method +
           "\",\"run_id\":\"" + run_id_wire(port.id) + "\"}";
}
bool ok(const Result<JsonValue> &response) {
    return response &&
           std::get<bool>(std::get<JsonValue::Object>(response.value().data).at("ok").data);
}
} // namespace
TEST_CASE("C11 thin projection preserves uint64 IDs and rejects unknown untrusted request fields") {
    MappingPort port;
    CallerFacade facade(port);
    auto context = caller();
    REQUIRE(parse_run_id(run_id_wire(port.id)).value() == port.id);
    REQUIRE_FALSE(parse_run_id("r.01.2.3"));
    REQUIRE_FALSE(parse_run_id("r.0.0.1"));
    REQUIRE_FALSE(parse_run_id("r.1.2.18446744073709551616"));
    auto response = facade.request(context, request(port));
    REQUIRE(ok(response));
    REQUIRE(port.statuses == 1);
    const auto encoded = canonical_json(response.value()).value();
    REQUIRE(encoded.find("18446744073709551615") != std::string::npos);
    auto forged = request(port);
    forged.insert(forged.size() - 1, ",\"subject\":\"admin\"");
    REQUIRE_FALSE(ok(facade.request(context, forged)));
    REQUIRE(port.statuses == 1);
    REQUIRE_FALSE(
        ok(facade.request(context, "{\"schema_version\":\"future/2\",\"method\":\"run.status\"}")));
    REQUIRE_FALSE(
        ok(facade.request(context, "{\"method\":\"run.status\",\"method\":\"run.cancel\"}")));
    REQUIRE(port.cancels == 0);
    context.subject.clear();
    REQUIRE_FALSE(ok(facade.request(context, request(port))));
    REQUIRE(port.statuses == 1);
}
TEST_CASE(
    "C11 JSON lines ingress drains oversized lines and disconnect never cancels accepted work") {
    MappingPort port;
    CallerFacade facade(port);
    const auto valid = request(port);
    std::istringstream input(std::string(301, 'x') + "\n" + valid + "\n");
    std::ostringstream output;
    REQUIRE(serve_json_lines(input, output, facade, caller(), 300) == 0);
    REQUIRE(port.statuses == 1);
    REQUIRE(port.cancels == 0);
    std::istringstream responses(output.str());
    std::string first, second;
    REQUIRE(static_cast<bool>(std::getline(responses, first)));
    REQUIRE(static_cast<bool>(std::getline(responses, second)));
    REQUIRE_FALSE(std::get<bool>(
        std::get<JsonValue::Object>(parse_bounded_json(first).value().data).at("ok").data));
    REQUIRE(std::get<bool>(
        std::get<JsonValue::Object>(parse_bounded_json(second).value().data).at("ok").data));
    std::istringstream empty;
    std::ostringstream discarded;
    REQUIRE(serve_json_lines(empty, discarded, facade, caller()) == 0);
    REQUIRE(port.cancels == 0);
}
TEST_CASE("C11 projection converts boundary exceptions to safe Runtime errors") {
    MappingPort port;
    port.throw_private = true;
    CallerFacade facade(port);
    std::istringstream input(request(port));
    std::ostringstream output;
    REQUIRE(serve_json_lines(input, output, facade, caller()) == 0);
    REQUIRE(output.str().find("internal_error") != std::string::npos);
    REQUIRE(output.str().find("private") == std::string::npos);
    REQUIRE(port.cancels == 0);
}
