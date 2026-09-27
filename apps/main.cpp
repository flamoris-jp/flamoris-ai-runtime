#include "flamoris/runtime/compiler.hpp"
#include "flamoris/runtime/facade.hpp"
#include "flamoris/runtime/registered_adapter.hpp"
#include "flamoris/runtime/runtime.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

using namespace flamoris::runtime;
namespace {
constexpr std::size_t input_bound = 1024 * 1024;

CapabilityContract identity_contract() {
    CapabilityContract contract(EffectSet::from_mask(1).value());
    contract.identifier = "algorithm.identity";
    contract.version = "1";
    contract.adapter_revision = "builtin.identity/1";
    contract.input_schema.kind = ValueSchema::Kind::object;
    ValueSchema text;
    text.kind = ValueSchema::Kind::string;
    text.max_bytes = 4096;
    contract.input_schema.properties.emplace("text", text);
    contract.input_schema.required.insert("text");
    contract.output_schema = contract.input_schema;
    contract.max_output_bytes = 8192;
    contract.max_timeout_ms = 10000;
    return contract;
}
CapabilitySnapshot registry() {
    CapabilitySnapshot result;
    auto contract = identity_contract();
    result.capabilities.emplace(contract.identifier, std::move(contract));
    return result;
}
std::set<AccessSurface> surfaces() {
    return {AccessSurface::status, AccessSurface::result,      AccessSurface::events,
            AccessSurface::trace,  AccessSurface::export_data, AccessSurface::replay,
            AccessSurface::cancel, AccessSurface::resume};
}
AuthorizationContext owner() {
    AuthorizationContext context;
    context.subject = "local-cli-owner";
    context.expires_at_ms = std::numeric_limits<std::uint64_t>::max();
    context.capabilities.insert("algorithm.identity");
    context.permitted_effects = 1;
    context.access = surfaces();
    return context;
}
int report(ErrorEnvelope error) {
    auto output = canonical_json(JsonValue::Object{{"schema_version", "flamoris.validation/1"},
                                                   {"valid", false},
                                                   {"error", error_projection(error)}});
    if (output)
        std::cerr << output.value() << '\n';
    return 2;
}
Result<std::string> read_submission(const std::filesystem::path &path) {
    std::error_code error;
    const auto bytes = std::filesystem::file_size(path, error);
    if (error || bytes > input_bound)
        return Result<std::string>::failure(ErrorEnvelope::make(ErrorCode::invalid_request));
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return Result<std::string>::failure(ErrorEnvelope::make(ErrorCode::invalid_request));
    std::string input;
    input.resize(static_cast<std::size_t>(bytes));
    file.read(input.data(), static_cast<std::streamsize>(input.size()));
    if (file.gcount() != static_cast<std::streamsize>(input.size()) ||
        file.peek() != std::char_traits<char>::eof())
        return Result<std::string>::failure(ErrorEnvelope::make(ErrorCode::invalid_request));
    return Result<std::string>::success(std::move(input));
}
int validate(const std::filesystem::path &path) {
    auto input = read_submission(path);
    if (!input)
        return report(input.error());
    auto compiled = Compiler{}.compile_submission(input.value(), registry());
    if (!compiled)
        return report(compiled.error());
    auto output = canonical_json(JsonValue::Object{
        {"schema_version", "flamoris.validation/1"},
        {"valid", true},
        {"fingerprint", compiled.value().plan->fingerprint},
        {"static_jobs", static_cast<double>(compiled.value().plan->static_jobs)},
        {"static_attempts", static_cast<double>(compiled.value().plan->static_attempts)}});
    if (!output)
        return report(output.error());
    std::cout << output.value() << '\n';
    return 0;
}
int serve() {
    RuntimeConfiguration config;
    config.capabilities = registry();
    config.policy.capabilities.insert("algorithm.identity");
    config.policy.permitted_effects = 1;
    config.policy.access = surfaces();
    RuntimeRegistration identity;
    identity.capability = "algorithm.identity";
    identity.provider = std::make_shared<IdentityProvider>();
    identity.requirements[ResourceKind::execution] = 1;
    identity.run_resource_limit[ResourceKind::execution] = 1;
    config.registrations.push_back(std::move(identity));
    // This standalone binding has no host authority. It never manufactures a
    // grant. Trusted embeddings provide enforced envelopes and matched receipts.
    config.host = std::make_shared<UnavailableHostAuthority>();
    auto runtime = RuntimeInstance::create(std::move(config));
    if (!runtime)
        return report(runtime.error());
    CallerFacade facade(*runtime.value());
    return serve_json_lines(std::cin, std::cout, facade, owner(), input_bound);
}
void usage(std::ostream &output) {
    output << "Usage: flamoris-runtime validate FILE\n"
              "       flamoris-runtime serve\n\n"
              "validate checks a bounded submission using the registered pure\n"
              "algorithm.identity capability and prints its plan fingerprint.\n"
              "serve accepts flamoris.control/1 JSON lines on stdin and emits\n"
              "flamoris.response/1 lines. This standalone binding has no host\n"
              "authority; execution waits for trusted host capacity and can time\n"
              "out. It does not load a model or start external services.\n";
}
} // namespace

int main(int argc, char **argv) {
    try {
        if (argc == 2 &&
            (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
            usage(std::cout);
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "validate")
            return validate(argv[2]);
        if (argc == 2 && std::string_view(argv[1]) == "serve")
            return serve();
        usage(std::cerr);
        return 2;
    } catch (const std::bad_alloc &) {
        return report(ErrorEnvelope::make(ErrorCode::resource_unavailable));
    } catch (...) {
        return report(ErrorEnvelope::make(ErrorCode::internal_error));
    }
}
