#include "flamoris/runtime/native.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace flamoris::runtime {
namespace {
template <class T> Result<T> fail(ErrorCode code, ErrorReason reason = ErrorReason::none) {
    return Result<T>::failure(ErrorEnvelope::make(code, ErrorStage::execution,
                                                  ExternalOutcome::not_applicable,
                                                  RetryDisposition::prohibited, reason));
}
} // namespace
Result<std::unique_ptr<NativeSession>>
NativeSession::create(std::shared_ptr<const TinyModel> model,
                      std::unique_ptr<ComputeImplementation> compute,
                      const ProcessorDefinition &processor, const TokenizerDefinition &tokenizer,
                      std::string_view prompt, NativeOptions options) {
    if (!model || !compute || processor != ProcessorDefinition{} ||
        !validate_tokenizer(tokenizer) || !options.max_output_tokens ||
        options.max_output_tokens > 128 || !options.max_input_bytes ||
        options.max_input_bytes > 255 || prompt.size() > options.max_input_bytes ||
        options.stop_sequences.size() > 8 || options.sampling.literal_grammar.size() > 127 ||
        !std::isfinite(options.sampling.temperature) || options.sampling.temperature < 0 ||
        !std::isfinite(options.sampling.repetition_penalty) ||
        options.sampling.repetition_penalty <= 0 || options.sampling.top_k > 258 ||
        !options.sampling.seed)
        return fail<std::unique_ptr<NativeSession>>(ErrorCode::invalid_request);
    for (const auto &stop : options.stop_sequences) {
        if (stop.empty() || stop.size() > 64 || !tokenize(stop, tokenizer, 64))
            return fail<std::unique_ptr<NativeSession>>(ErrorCode::invalid_request);
    }
    if (!options.sampling.literal_grammar.empty() &&
        (!tokenize(options.sampling.literal_grammar, tokenizer, 127) || options.injection_slot))
        return fail<std::unique_ptr<NativeSession>>(ErrorCode::invalid_request,
                                                    ErrorReason::unsupported_operation);
    auto tokens = tokenize(prompt, tokenizer, options.max_input_bytes);
    if (!tokens)
        return Result<std::unique_ptr<NativeSession>>::failure(tokens.error());
    if (tokens.value().size() + 1 + options.max_output_tokens > model->definition().context)
        return fail<std::unique_ptr<NativeSession>>(ErrorCode::resource_unavailable);
    try {
        auto session = std::unique_ptr<NativeSession>(new NativeSession);
        session->state_.pins = {model->definition().identity + ":" +
                                    model->definition().artifact_sha256,
                                processor.identity + ":" + processor.fingerprint,
                                tokenizer.identity + ":" + tokenizer.fingerprint,
                                session->profile_.identity, compute->device().identity};
        session->model_ = std::move(model);
        session->compute_ = std::move(compute);
        session->tokenizer_ = tokenizer;
        session->options_ = std::move(options);
        session->causal_.rng = session->options_.sampling.seed;
        auto &state = session->causal_;
        state.input.reserve(256);
        state.evaluated.reserve(256);
        state.emitted.reserve(128);
        state.keys.reserve(256 * 8);
        state.values.reserve(256 * 8);
        state.logits.reserve(258);
        state.output.reserve(128);
        state.stop_tail.reserve(64);
        state.injection_commands.reserve(32);
        state.input.push_back(tokenizer.bos);
        state.input.insert(state.input.end(), tokens.value().begin(), tokens.value().end());
        for (auto token : state.input)
            ++state.penalty[token];
        return Result<std::unique_ptr<NativeSession>>::success(std::move(session));
    } catch (const std::bad_alloc &) {
        return fail<std::unique_ptr<NativeSession>>(ErrorCode::resource_unavailable);
    }
}
Result<void> NativeSession::evaluate_one(TokenId token) {
    auto logits = model_->evaluate(token, causal_.keys, causal_.values, *compute_);
    if (!logits) {
        state_.valid = false;
        state_.stage = NativeStage::failed;
        return Result<void>::failure(logits.error());
    }
    causal_.logits = std::move(logits).value();
    causal_.evaluated.push_back(token);
    return Result<void>::success();
}
Result<TokenId> NativeSession::sample() {
    if (causal_.logits.size() != 258)
        return fail<TokenId>(ErrorCode::state_unavailable);
    std::vector<std::pair<TokenId, double>> candidates;
    candidates.reserve(257);
    const auto &sampling = options_.sampling;
    for (TokenId token = 0; token < 258; ++token) {
        if (token == tokenizer_.bos)
            continue;
        if (!sampling.literal_grammar.empty()) {
            const auto expected = causal_.grammar_position < sampling.literal_grammar.size()
                                      ? static_cast<TokenId>(static_cast<unsigned char>(
                                            sampling.literal_grammar[causal_.grammar_position]))
                                      : tokenizer_.eos;
            if (token != expected)
                continue;
        }
        auto decoder = causal_.decoder;
        if (token == tokenizer_.eos) {
            if (!decoder.append({}, true))
                continue;
        } else {
            const char byte = static_cast<char>(token);
            if (!decoder.append(std::string_view(&byte, 1)))
                continue;
            // A budget boundary cannot silently discard a partial Unicode scalar.
            if (causal_.emitted.size() + 1 == options_.max_output_tokens && !decoder.carry.empty())
                continue;
        }
        auto score = static_cast<double>(causal_.logits[token]);
        if (!std::isfinite(score))
            return fail<TokenId>(ErrorCode::native_execution_failure);
        if (causal_.penalty[token])
            score = score < 0 ? score * sampling.repetition_penalty
                              : score / sampling.repetition_penalty;
        candidates.emplace_back(token, score);
    }
    if (candidates.empty())
        return fail<TokenId>(ErrorCode::invalid_result);
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](auto a, auto b) { return a.second > b.second; });
    if (sampling.top_k && candidates.size() > sampling.top_k)
        candidates.resize(sampling.top_k);
    TokenId selected = candidates.front().first;
    if (sampling.temperature > 0) {
        const auto maximum = candidates.front().second;
        double total = 0;
        for (auto &[token, score] : candidates) {
            (void)token;
            score = std::exp((score - maximum) / sampling.temperature);
            total += score;
        }
        // Explicit xorshift64* algorithm and 53-bit mapping are part of this profile.
        auto rng = causal_.rng;
        rng ^= rng >> 12;
        rng ^= rng << 25;
        rng ^= rng >> 27;
        causal_.rng = rng;
        ++causal_.rng_draws;
        const auto bits = rng * UINT64_C(2685821657736338717);
        const double uniform = static_cast<double>(bits >> 11) * (1.0 / 9007199254740992.0);
        double cursor = uniform * total;
        selected = candidates.back().first;
        for (const auto &[token, score] : candidates) {
            cursor -= score;
            if (cursor <= 0) {
                selected = token;
                break;
            }
        }
    }
    ++causal_.penalty[selected];
    ++causal_.grammar_position;
    return Result<TokenId>::success(selected);
}
Result<SegmentReceipt> NativeSession::step(std::size_t max_prefill_tokens) {
    if (!max_prefill_tokens || max_prefill_tokens > 32)
        return fail<SegmentReceipt>(ErrorCode::invalid_request);
    if (!state_.valid || state_.paused ||
        (state_.stage != NativeStage::prefill && state_.stage != NativeStage::decode))
        return fail<SegmentReceipt>(ErrorCode::state_unavailable);
    if (state_.version == std::numeric_limits<std::uint64_t>::max())
        return fail<SegmentReceipt>(ErrorCode::budget_exceeded, ErrorReason::counter_exhausted);
    SegmentReceipt receipt;
    try {
        if (state_.stage == NativeStage::prefill) {
            if (causal_.pending) {
                auto evaluated = evaluate_one(*causal_.pending);
                if (!evaluated)
                    return Result<SegmentReceipt>::failure(evaluated.error());
                causal_.pending.reset();
                ++receipt.evaluated_tokens;
            }
            const auto count = std::min(max_prefill_tokens - receipt.evaluated_tokens,
                                        causal_.input.size() - state_.input_progress);
            for (std::size_t i = 0; i < count; ++i) {
                auto evaluated = evaluate_one(causal_.input[state_.input_progress]);
                if (!evaluated)
                    return Result<SegmentReceipt>::failure(evaluated.error());
                ++state_.input_progress;
                ++receipt.evaluated_tokens;
            }
            if (state_.input_progress == causal_.input.size())
                state_.stage = NativeStage::decode;
        } else {
            if (causal_.pending) {
                auto evaluated = evaluate_one(*causal_.pending);
                if (!evaluated)
                    return Result<SegmentReceipt>::failure(evaluated.error());
                causal_.pending.reset();
                ++receipt.evaluated_tokens;
            }
            auto selected = sample();
            if (!selected) {
                state_.valid = false;
                state_.stage = NativeStage::failed;
                return Result<SegmentReceipt>::failure(selected.error());
            }
            const auto token = selected.value();
            causal_.emitted.push_back(token);
            causal_.pending = token;
            receipt.token = token;
            ++state_.output_progress;
            if (token != tokenizer_.eos) {
                const char byte = static_cast<char>(token);
                auto decoded = causal_.decoder.append(std::string_view(&byte, 1));
                if (!decoded) {
                    state_.valid = false;
                    state_.stage = NativeStage::failed;
                    return Result<SegmentReceipt>::failure(decoded.error());
                }
                receipt.text = std::move(decoded).value();
                causal_.output += receipt.text;
                causal_.stop_tail += receipt.text;
                for (const auto &stop : options_.stop_sequences)
                    if (causal_.stop_tail.ends_with(stop))
                        causal_.stop_matched = true;
                if (causal_.stop_tail.size() > 64)
                    causal_.stop_tail.erase(0, causal_.stop_tail.size() - 64);
            }
            if (token == tokenizer_.eos || causal_.stop_matched ||
                causal_.emitted.size() == options_.max_output_tokens) {
                if (!causal_.decoder.append({}, true)) {
                    state_.valid = false;
                    state_.stage = NativeStage::failed;
                    return fail<SegmentReceipt>(ErrorCode::invalid_result);
                }
                state_.stage = NativeStage::completing;
            }
        }
        auto synchronized = compute_->synchronize();
        if (!synchronized) {
            state_.valid = false;
            state_.stage = NativeStage::failed;
            return Result<SegmentReceipt>::failure(synchronized.error());
        }
        receipt.state_version = ++state_.version;
        receipt.quiescent = compute_->receipt().quiescent;
        receipt.state_valid = state_.valid;
        receipt.complete = state_.stage == NativeStage::completing;
        return Result<SegmentReceipt>::success(std::move(receipt));
    } catch (const std::bad_alloc &) {
        state_.valid = false;
        state_.stage = NativeStage::failed;
        return fail<SegmentReceipt>(ErrorCode::resource_unavailable);
    }
}
Result<std::uint64_t> NativeSession::pause() {
    if (!state_.valid || state_.paused ||
        (state_.stage != NativeStage::prefill && state_.stage != NativeStage::decode))
        return fail<std::uint64_t>(ErrorCode::state_unavailable);
    if (state_.suspension_generation == std::numeric_limits<std::uint64_t>::max())
        return fail<std::uint64_t>(ErrorCode::budget_exceeded, ErrorReason::counter_exhausted);
    auto receipt = compute_->synchronize();
    if (!receipt || !compute_->receipt().quiescent)
        return fail<std::uint64_t>(ErrorCode::cleanup_timeout);
    state_.paused = true;
    return Result<std::uint64_t>::success(++state_.suspension_generation);
}
Result<void> NativeSession::resume(const NativePins &pins, std::uint64_t generation) {
    if (!state_.valid || !state_.paused || pins != state_.pins ||
        generation != state_.suspension_generation)
        return fail<void>(ErrorCode::state_unavailable, ErrorReason::version_mismatch);
    state_.paused = false;
    return Result<void>::success();
}
Result<void> NativeSession::inject(std::string_view input, std::uint64_t command_id) {
    if (!state_.valid || !options_.injection_slot || !command_id ||
        !options_.sampling.literal_grammar.empty() ||
        (state_.stage != NativeStage::prefill && state_.stage != NativeStage::decode))
        return fail<void>(ErrorCode::invalid_request, ErrorReason::unsupported_operation);
    if (std::find(causal_.injection_commands.begin(), causal_.injection_commands.end(),
                  command_id) != causal_.injection_commands.end())
        return Result<void>::success();
    auto tokens = tokenize(input, tokenizer_, options_.max_input_bytes);
    if (!tokens || tokens.value().empty())
        return fail<void>(ErrorCode::invalid_request);
    if (causal_.injection_commands.size() >= 32 ||
        causal_.input.size() + tokens.value().size() + options_.max_output_tokens >
            profile_.max_context ||
        state_.version == std::numeric_limits<std::uint64_t>::max())
        return fail<void>(ErrorCode::budget_exceeded);
    // Queue approved input at this safe point; the next permitted prefill evaluates
    // an already emitted token first. In particular, injection while paused runs no compute.
    causal_.input.insert(causal_.input.end(), tokens.value().begin(), tokens.value().end());
    for (auto token : tokens.value())
        ++causal_.penalty[token];
    causal_.injection_commands.push_back(command_id);
    state_.stage = NativeStage::prefill;
    ++state_.version;
    return Result<void>::success();
}
Result<void> NativeSession::request_stop(bool graceful) {
    if (state_.stage == NativeStage::released)
        return fail<void>(ErrorCode::state_unavailable);
    if (graceful && (!options_.allow_partial_output || !causal_.decoder.append({}, true)))
        return fail<void>(ErrorCode::invalid_request, ErrorReason::unsupported_operation);
    auto synchronized = compute_->synchronize();
    if (!synchronized || !compute_->receipt().quiescent)
        return fail<void>(ErrorCode::cleanup_timeout);
    state_.paused = false;
    state_.valid = false;
    state_.stage = graceful ? NativeStage::completing : NativeStage::stopped;
    return Result<void>::success();
}
Result<NativeReleaseReceipt> NativeSession::release() {
    if (state_.stage == NativeStage::released)
        return Result<NativeReleaseReceipt>::success({state_.version, 0, true});
    if (state_.stage != NativeStage::stopped && state_.stage != NativeStage::completing &&
        state_.stage != NativeStage::failed)
        return fail<NativeReleaseReceipt>(ErrorCode::state_unavailable);
    auto synchronized = compute_->synchronize();
    if (!synchronized || !compute_->receipt().quiescent)
        return fail<NativeReleaseReceipt>(ErrorCode::cleanup_timeout);
    const auto bytes = retained_bytes();
    causal_ = CausalTextState{};
    model_.reset();
    compute_.reset();
    state_.stage = NativeStage::released;
    state_.valid = false;
    state_.paused = false;
    return Result<NativeReleaseReceipt>::success({state_.version, bytes, true});
}
Result<void> NativeSession::unsupported_control(std::string_view) const {
    return fail<void>(ErrorCode::invalid_request, ErrorReason::unsupported_operation);
}
std::size_t NativeSession::retained_bytes() const noexcept {
    return causal_.input.capacity() * sizeof(TokenId) +
           causal_.evaluated.capacity() * sizeof(TokenId) +
           causal_.emitted.capacity() * sizeof(TokenId) +
           (causal_.keys.capacity() + causal_.values.capacity() + causal_.logits.capacity()) *
               sizeof(float) +
           causal_.output.capacity() + causal_.stop_tail.capacity() +
           causal_.decoder.carry.capacity() +
           causal_.injection_commands.capacity() * sizeof(std::uint64_t) + sizeof(CausalTextState);
}
} // namespace flamoris::runtime
