#include "flamoris/runtime/facade.hpp"
#include <algorithm>
#include <array>
#include <istream>
#include <ostream>
#include <set>

namespace flamoris::runtime {
namespace {
using Object = JsonValue::Object;
using Array = JsonValue::Array;
ErrorEnvelope invalid() noexcept { return ErrorEnvelope::make(ErrorCode::invalid_request); }
std::string instance_wire(RuntimeInstanceId instance) {
    return "i." + std::to_string(instance.high()) + "." + std::to_string(instance.low());
}
std::string job_wire(JobId job) {
    return run_id_wire(job.run()) + ".j." + std::to_string(job.value());
}
std::string activity_name(RunActivity state) {
    switch (state) {
    case RunActivity::created:
        return "created";
    case RunActivity::queued:
        return "queued";
    case RunActivity::running:
        return "running";
    case RunActivity::waiting:
        return "waiting";
    case RunActivity::paused:
        return "paused";
    case RunActivity::cancelling:
        return "cancelling";
    case RunActivity::finalizing:
        return "finalizing";
    case RunActivity::succeeded:
        return "succeeded";
    case RunActivity::failed:
        return "failed";
    case RunActivity::cancelled:
        return "cancelled";
    }
    return "unavailable";
}
const std::string *string_field(const Object &object, const char *name) {
    auto it = object.find(name);
    return it == object.end() ? nullptr : std::get_if<std::string>(&it->second.data);
}
bool keys(const Object &object, std::initializer_list<const char *> required,
          std::initializer_list<const char *> optional = {}) {
    for (const auto *key : required)
        if (!object.contains(key))
            return false;
    for (const auto &[key, unused] : object) {
        (void)unused;
        if (std::none_of(required.begin(), required.end(),
                         [&](const char *k) { return key == k; }) &&
            std::none_of(optional.begin(), optional.end(), [&](const char *k) { return key == k; }))
            return false;
    }
    return true;
}
JsonValue success(JsonValue value) {
    return Object{
        {"schema_version", "flamoris.response/1"}, {"ok", true}, {"value", std::move(value)}};
}
JsonValue failure(const ErrorEnvelope &error) {
    return Object{{"schema_version", "flamoris.response/1"},
                  {"ok", false},
                  {"error", error_projection(error)}};
}
template <class T, class Encode> Result<JsonValue> project(Result<T> result, Encode encode) {
    if (!result)
        return Result<JsonValue>::success(failure(result.error()));
    return Result<JsonValue>::success(success(encode(result.value())));
}
JsonValue command_projection(const CommandReceipt &receipt) {
    return Object{{"accepted", receipt.accepted},
                  {"applied", receipt.applied},
                  {"terminal", receipt.terminal}};
}
JsonValue status_projection(const RunSnapshot &snapshot) {
    Array jobs;
    for (const auto &job : snapshot.jobs) {
        Object value{{"job_id", job_wire(job.id)},
                     {"state", state_name(job.state)},
                     {"continuation", job.continuation},
                     {"pending_resume", job.pending_resume},
                     {"execution_in_flight", job.execution_in_flight},
                     {"cleanup_pending", job.cleanup_pending},
                     {"pause_requested", job.pause_requested},
                     {"pause_supported", job.pause_supported}};
        if (job.parent)
            value.emplace("parent_job_id", job_wire(*job.parent));
        if (job.terminal_intent)
            value.emplace("terminal_intent", state_name(*job.terminal_intent));
        if (job.error)
            value.emplace("error_code", std::string(to_string(*job.error)));
        jobs.emplace_back(std::move(value));
    }
    return Object{{"run_id", run_id_wire(snapshot.id)},
                  {"root_job_id", job_wire(snapshot.root)},
                  {"state", activity_name(snapshot.activity)},
                  {"watermark", std::to_string(snapshot.watermark)},
                  {"dispatch_open", snapshot.dispatch_open},
                  {"child_creation_open", snapshot.child_creation_open},
                  {"pause_barrier_pending", snapshot.pause_barrier_pending},
                  {"jobs", std::move(jobs)}};
}
JsonValue replay_projection(const ReplaySnapshot &snapshot) {
    Array jobs;
    for (const auto &job : snapshot.jobs)
        jobs.emplace_back(Object{{"job_id", job_wire(job.job)}, {"state", state_name(job.state)}});
    Object result{{"playback", true},
                  {"complete", snapshot.complete},
                  {"watermark", std::to_string(snapshot.watermark)},
                  {"applied_groups", std::to_string(snapshot.applied_groups)},
                  {"rejected_groups", std::to_string(snapshot.rejected_groups)},
                  {"jobs", std::move(jobs)}};
    if (snapshot.activity)
        result.emplace("state", activity_name(*snapshot.activity));
    return result;
}
} // namespace

std::string run_id_wire(RunId id) {
    return "r." + std::to_string(id.instance().high()) + "." + std::to_string(id.instance().low()) +
           "." + std::to_string(id.value());
}
Result<RunId> parse_run_id(std::string_view text) {
    if (text.size() > 128 || !text.starts_with("r."))
        return Result<RunId>::failure(invalid());
    text.remove_prefix(2);
    std::array<std::uint64_t, 3> parts{};
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto split = text.find('.');
        if ((i < 2 && split == std::string_view::npos) ||
            (i == 2 && split != std::string_view::npos))
            return Result<RunId>::failure(invalid());
        auto number = parse_counter(text.substr(0, split));
        if (!number)
            return Result<RunId>::failure(number.error());
        parts[i] = number.value();
        if (i < 2)
            text.remove_prefix(split + 1);
    }
    RunId id{RuntimeInstanceId{parts[0], parts[1]}, parts[2]};
    if (!id.valid())
        return Result<RunId>::failure(invalid());
    return Result<RunId>::success(id);
}
JsonValue error_projection(const ErrorEnvelope &error) {
    Array causes;
    for (const auto cause : error.cause_codes())
        causes.emplace_back(std::string(to_string(cause)));
    Object result{{"schema_version", "flamoris.error/1"},
                  {"code", std::string(to_string(error.code()))},
                  {"category", std::string(to_string(error.category()))},
                  {"message", std::string(error.message())},
                  {"stage", std::string(to_string(error.stage()))},
                  {"external_outcome", std::string(to_string(error.external_outcome()))},
                  {"retry_disposition", std::string(to_string(error.retry_disposition()))},
                  {"cause_codes", std::move(causes)}};
    if (error.reason() != ErrorReason::none)
        result.emplace("reason", std::string(to_string(error.reason())));
    return result;
}
JsonValue observation_projection(const ObservationPage &page) {
    Array events;
    for (const auto &group : page.groups) {
        std::size_t index = 0;
        for (const auto &event : group.events) {
            Object payload;
            if (event.from)
                payload.emplace("from", state_name(*event.from));
            if (event.to)
                payload.emplace("to", state_name(*event.to));
            if (event.run_from)
                payload.emplace("from", activity_name(*event.run_from));
            if (event.run_to)
                payload.emplace("to", activity_name(*event.run_to));
            if (event.error)
                payload.emplace("error_code", std::string(to_string(*event.error)));
            if (event.generation)
                payload.emplace("generation", std::to_string(event.generation));
            if (event.operation)
                payload.emplace("operation", std::to_string(event.operation));
            if (event.ledger_revision)
                payload.emplace("ledger_revision", std::to_string(event.ledger_revision));
            if (event.external_outcome)
                payload.emplace("external_outcome",
                                std::string(to_string(*event.external_outcome)));
            if (event.dispatch_generation)
                payload.emplace("dispatch_generation", event.dispatch_generation->to_wire());
            Object envelope{
                {"schema_version", "flamoris.event/1"},
                {"kind", event.kind},
                {"runtime_instance_id", instance_wire(event.job.instance())},
                {"run_id", run_id_wire(event.job.run())},
                {"job_id", job_wire(event.job)},
                {"seq", std::to_string(event.sequence)},
                {"event_id", run_id_wire(event.job.run()) + ".e." + std::to_string(event.sequence)},
                {"transition_id", std::to_string(group.transition)},
                {"group_index", static_cast<double>(index++)},
                {"group_size", static_cast<double>(group.events.size())},
                {"monotonic_offset", std::to_string(event.monotonic_offset)},
                {"wall_time", nullptr},
                {"payload", std::move(payload)}};
            if (event.attempt_id)
                envelope.emplace("attempt_id", event.attempt_id->to_wire());
            if (event.command_id)
                envelope.emplace("command_id", event.command_id->to_wire());
            events.emplace_back(std::move(envelope));
        }
    }
    const char *state = page.stream_state == ObservationStreamState::open     ? "open"
                        : page.stream_state == ObservationStreamState::closed ? "closed"
                                                                              : "expired";
    Object result{{"events", std::move(events)},
                  {"earliest_retained_seq", std::to_string(page.earliest_retained_sequence)},
                  {"watermark", std::to_string(page.watermark)},
                  {"next_after", std::to_string(page.next_after)},
                  {"stream_state", state},
                  {"telemetry_dropped", std::to_string(page.telemetry_dropped)}};
    if (page.gap)
        result.emplace("gap",
                       Object{{"requested_after", std::to_string(page.gap->requested_after)},
                              {"earliest_available", std::to_string(page.gap->earliest_available)},
                              {"watermark", std::to_string(page.gap->watermark)},
                              {"subscriber_overflow", page.gap->subscriber_overflow},
                              {"expired", page.gap->expired}});
    if (page.incomplete_group)
        result.emplace(
            "incomplete_group",
            Object{{"transition_id", std::to_string(page.incomplete_group->transition)},
                   {"first_sequence", std::to_string(page.incomplete_group->first_sequence)},
                   {"expected_events", std::to_string(page.incomplete_group->expected_events)},
                   {"available_events", std::to_string(page.incomplete_group->available_events)}});
    return result;
}

Result<JsonValue> CallerFacade::request(const AuthorizationContext &caller, std::string_view json) {
    try {
        if (caller.subject.empty() || caller.revoked)
            return Result<JsonValue>::success(
                failure(ErrorEnvelope::make(ErrorCode::permission_denied)));
        auto parsed = parse_bounded_json(json);
        if (!parsed)
            return Result<JsonValue>::success(failure(parsed.error()));
        auto *object = std::get_if<Object>(&parsed.value().data);
        if (!object)
            return Result<JsonValue>::success(failure(invalid()));
        const auto *version = string_field(*object, "schema_version");
        const auto *method = string_field(*object, "method");
        if (!version || *version != "flamoris.control/1" || !method)
            return Result<JsonValue>::success(failure(invalid()));
        if (*method == "run.submit") {
            if (!keys(*object, {"schema_version", "method", "submission"}))
                return Result<JsonValue>::success(failure(invalid()));
            auto normalized = canonical_json(object->at("submission"));
            if (!normalized)
                return Result<JsonValue>::success(failure(normalized.error()));
            return project(runtime_.submit(caller, normalized.value()),
                           [](const auto &receipt) -> JsonValue {
                               return Object{{"run_id", run_id_wire(receipt.id)},
                                             {"duplicate", receipt.duplicate}};
                           });
        }
        const auto *run_text = string_field(*object, "run_id");
        if (!run_text)
            return Result<JsonValue>::success(failure(invalid()));
        auto run = parse_run_id(*run_text);
        if (!run)
            return Result<JsonValue>::success(failure(run.error()));
        if (*method == "events.read") {
            if (!keys(*object, {"schema_version", "method", "run_id", "after"}, {"limit"}))
                return Result<JsonValue>::success(failure(invalid()));
            const auto *after_text = string_field(*object, "after");
            if (!after_text)
                return Result<JsonValue>::success(failure(invalid()));
            auto after = parse_counter(*after_text);
            if (!after)
                return Result<JsonValue>::success(failure(after.error()));
            std::size_t limit = 256;
            if (object->contains("limit")) {
                const auto *limit_text = string_field(*object, "limit");
                if (!limit_text)
                    return Result<JsonValue>::success(failure(invalid()));
                auto parsed_limit = parse_counter(*limit_text);
                if (!parsed_limit || !parsed_limit.value() || parsed_limit.value() > 256)
                    return Result<JsonValue>::success(failure(invalid()));
                limit = static_cast<std::size_t>(parsed_limit.value());
            }
            return project(runtime_.events(caller, run.value(), after.value(), limit),
                           observation_projection);
        }
        if (*method == "run.pause" || *method == "run.resume") {
            if (!keys(*object, {"schema_version", "method", "run_id", "command_id"}))
                return Result<JsonValue>::success(failure(invalid()));
            const auto *command_text = string_field(*object, "command_id");
            if (!command_text)
                return Result<JsonValue>::success(failure(invalid()));
            auto command = parse_counter(*command_text);
            if (!command || !command.value())
                return Result<JsonValue>::success(failure(invalid()));
            return project(*method == "run.pause"
                               ? runtime_.pause(caller, run.value(), command.value())
                               : runtime_.resume(caller, run.value(), command.value()),
                           command_projection);
        }
        if (!keys(*object, {"schema_version", "method", "run_id"}))
            return Result<JsonValue>::success(failure(invalid()));
        if (*method == "run.status")
            return project(runtime_.status(caller, run.value()), status_projection);
        if (*method == "run.cancel")
            return project(runtime_.cancel(caller, run.value()), command_projection);
        if (*method == "trace.replay")
            return project(runtime_.replay(caller, run.value()), replay_projection);
        if (*method == "run.result")
            return project(runtime_.result(caller, run.value()),
                           [](const RunResult &result) -> JsonValue {
                               Object value{{"pending", result.pending},
                                            {"external_outcome",
                                             std::string(to_string(result.external_outcome))}};
                               if (result.value)
                                   value.emplace("result", *result.value);
                               if (result.error)
                                   value.emplace("error", error_projection(*result.error));
                               return value;
                           });
        return Result<JsonValue>::success(failure(invalid()));
    } catch (...) {
        return Result<JsonValue>::failure(ErrorEnvelope::make(ErrorCode::internal_error));
    }
}

int serve_json_lines(std::istream &input, std::ostream &output, CallerFacade &facade,
                     const AuthorizationContext &context, std::size_t max_line_bytes) {
    if (!max_line_bytes || max_line_bytes > 1048576)
        return 2;
    try {
        std::string line;
        line.reserve(std::min<std::size_t>(max_line_bytes, 4096));
        bool oversized = false;
        auto respond = [&]() {
            auto response = oversized ? Result<JsonValue>::success(failure(invalid()))
                                      : facade.request(context, line);
            auto encoded = canonical_json(response ? response.value() : failure(response.error()));
            if (!encoded)
                return false;
            output << encoded.value() << '\n';
            output.flush();
            line.clear();
            oversized = false;
            return output.good();
        };
        char value;
        while (input.get(value)) {
            if (value == '\n') {
                if (!respond())
                    return 1;
            } else if (line.size() < max_line_bytes && !oversized)
                line.push_back(value);
            else
                oversized = true;
        }
        if ((!line.empty() || oversized) && !respond())
            return 1;
        return input.bad() ? 1 : 0;
    } catch (...) {
        return 1;
    }
}
} // namespace flamoris::runtime
