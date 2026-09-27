#include "flamoris/runtime/compiler.hpp"
#include "flamoris/runtime/facade.hpp"
#include "flamoris/runtime/native.hpp"
#include "flamoris/runtime/registered_adapter.hpp"
#include "flamoris/runtime/runtime.hpp"
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

using namespace flamoris::runtime;
namespace {
constexpr std::size_t input_bound = 1024 * 1024;
constexpr auto fixture_sha256 =
    "5cd5a0bacb4cce1efd801fe2a4d7c1347467538ef6e393b37d955d2499ef74e2";

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

struct GenerateArguments {
    std::string prompt;
    float temperature{0.0F};
    std::uint64_t seed{1};
    std::size_t max_tokens{32};
    std::filesystem::path model =
        std::filesystem::path(FLAMORIS_SOURCE_DIR) / "fixtures/native/tiny-causal-v1.bin";
};

template <class T> bool parse_number(std::string_view text, T &value) {
    const auto *first = text.data();
    const auto *last = first + text.size();
    const auto [ptr, error] = std::from_chars(first, last, value);
    return error == std::errc{} && ptr == last;
}

Result<GenerateArguments> parse_generate_arguments(int argc, char **argv) {
    GenerateArguments result;
    bool prompt_seen = false;
    for (int index = 2; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (index + 1 >= argc)
            return Result<GenerateArguments>::failure(
                ErrorEnvelope::make(ErrorCode::invalid_request));
        const std::string_view value(argv[++index]);
        if (argument == "--prompt") {
            result.prompt = std::string(value);
            prompt_seen = true;
        } else if (argument == "--temperature") {
            if (!parse_number(value, result.temperature) || !std::isfinite(result.temperature) ||
                result.temperature < 0.0F)
                return Result<GenerateArguments>::failure(
                    ErrorEnvelope::make(ErrorCode::invalid_request));
        } else if (argument == "--seed") {
            if (!parse_number(value, result.seed))
                return Result<GenerateArguments>::failure(
                    ErrorEnvelope::make(ErrorCode::invalid_request));
        } else if (argument == "--max-tokens") {
            if (!parse_number(value, result.max_tokens) || result.max_tokens == 0)
                return Result<GenerateArguments>::failure(
                    ErrorEnvelope::make(ErrorCode::invalid_request));
        } else if (argument == "--model") {
            result.model = std::filesystem::path(value);
        } else {
            return Result<GenerateArguments>::failure(
                ErrorEnvelope::make(ErrorCode::invalid_request));
        }
    }
    if (!prompt_seen)
        return Result<GenerateArguments>::failure(
            ErrorEnvelope::make(ErrorCode::invalid_request));
    return Result<GenerateArguments>::success(std::move(result));
}

int emit_dev_event(JsonValue::Object event) {
    event.emplace("schema_version", "flamoris.dev.native-event/1");
    auto output = canonical_json(std::move(event));
    if (!output)
        return report(output.error());
    std::cout << output.value() << '\n';
    return 0;
}

int generate(int argc, char **argv) {
    auto arguments = parse_generate_arguments(argc, argv);
    if (!arguments)
        return report(arguments.error());

    auto model = TinyModel::load(arguments.value().model, fixture_sha256);
    if (!model)
        return report(model.error());

    NativeOptions options;
    options.max_output_tokens = arguments.value().max_tokens;
    options.sampling.temperature = arguments.value().temperature;
    options.sampling.seed = arguments.value().seed;

    auto created = NativeSession::create(model.value(), make_cpu_compute(), {}, {},
                                         arguments.value().prompt, options);
    if (!created)
        return report(created.error());
    auto session = std::move(created).value();

    std::uint64_t sequence = 0;
    if (emit_dev_event(JsonValue::Object{
            {"kind", "generation.started"},
            {"sequence", static_cast<double>(++sequence)},
            {"model", model.value()->definition().identity},
            {"profile", session->profile().identity}}) != 0)
        return 2;

    while (session->state().stage == NativeStage::prefill ||
           session->state().stage == NativeStage::decode) {
        auto receipt = session->step(8);
        if (!receipt)
            return report(receipt.error());
        if (receipt.value().token) {
            JsonValue::Object event{{"kind", "token.generated"},
                                    {"sequence", static_cast<double>(++sequence)},
                                    {"token", static_cast<double>(*receipt.value().token)},
                                    {"text", receipt.value().text}};
            if (emit_dev_event(std::move(event)) != 0)
                return 2;
        }
    }

    const auto output_text = session->causal_state().output;
    const auto emitted_tokens = session->causal_state().emitted.size();
    auto released = session->release();
    if (!released)
        return report(released.error());

    return emit_dev_event(JsonValue::Object{
        {"kind", "generation.completed"},
        {"sequence", static_cast<double>(++sequence)},
        {"output", output_text},
        {"emitted_tokens", static_cast<double>(emitted_tokens)}});
}

void usage(std::ostream &output) {
    output << "Usage: flamoris-runtime validate FILE\n"
              "       flamoris-runtime serve\n"
              "       flamoris-runtime generate --prompt TEXT [--temperature FLOAT]\n"
              "                                [--seed UINT] [--max-tokens UINT]\n"
              "                                [--model FILE]\n\n"
              "validate checks a bounded submission using the registered pure\n"
              "algorithm.identity capability and prints its plan fingerprint.\n"
              "serve accepts flamoris.control/1 JSON lines on stdin and emits\n"
              "flamoris.response/1 lines. This standalone binding has no host\n"
              "authority; execution waits for trusted host capacity and can time\n"
              "out. It does not load a model or start external services.\n"
              "generate is an explicit developer smoke-test path. It loads the\n"
              "self-authored tiny native fixture on CPU and emits JSONL events.\n"
              "It bypasses Runtime host admission and must not be treated as a\n"
              "production host authority or general model-serving interface.\n";
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
        if (argc >= 2 && std::string_view(argv[1]) == "generate")
            return generate(argc, argv);
        usage(std::cerr);
        return 2;
    } catch (const std::bad_alloc &) {
        return report(ErrorEnvelope::make(ErrorCode::resource_unavailable));
    } catch (...) {
        return report(ErrorEnvelope::make(ErrorCode::internal_error));
    }
}
