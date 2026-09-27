#pragma once
#include "flamoris/runtime/compute.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace flamoris::runtime {
using TokenId = std::uint32_t;
struct TokenizerDefinition {
    std::string identity{"flamoris.byte-utf8.v1"};
    std::string fingerprint{"byte-identity-strict-utf8-bos256-eos257-v1"};
    TokenId bos{256}, eos{257};
    bool normalization{false};
    bool operator==(const TokenizerDefinition &) const = default;
};
struct ProcessorDefinition {
    std::string identity{"flamoris.raw-prompt.v1"};
    std::string fingerprint{"raw-utf8-prefix-empty-suffix-empty-v1"};
    std::string tokenizer_identity{"flamoris.byte-utf8.v1"};
    std::string prefix, suffix;
    bool operator==(const ProcessorDefinition &) const = default;
};
struct NativePins {
    std::string model, processor, tokenizer, profile, compute;
    bool operator==(const NativePins &) const = default;
};
struct NativeProfile {
    std::string identity{"flamoris.tiny-causal-fp32.v1"};
    std::size_t max_context{256}, max_input_bytes{255}, max_output_tokens{128};
    bool pause{true}, inject{true}, graceful_stop{true};
    bool offload{false}, snapshot{false}, rewind{false}, batching{false}, partial_step{false};
};
Result<void> validate_tokenizer(const TokenizerDefinition &);
Result<std::vector<TokenId>> tokenize(std::string_view, const TokenizerDefinition &,
                                      std::size_t max_bytes);
Result<std::string> detokenize(std::span<const TokenId>, const TokenizerDefinition &);
Result<std::string> utf16_to_utf8(std::u16string_view, std::size_t max_bytes);
// Copyable value for complete continuation state; incomplete bytes are never emitted.
struct Utf8Decoder {
    std::string carry;
    Result<std::string> append(std::string_view bytes, bool final = false);
    bool operator==(const Utf8Decoder &) const = default;
};
struct ModelDefinition {
    std::string identity{"flamoris.tiny-causal.v1"};
    std::string artifact_sha256;
    std::size_t vocabulary{258}, width{8}, context{256};
};
class TinyModel {
  public:
    static Result<std::shared_ptr<const TinyModel>>
    load(const std::filesystem::path &registered_path, std::string_view expected_sha256);
    const ModelDefinition &definition() const noexcept { return definition_; }
    std::size_t resident_bytes() const noexcept { return weights_.capacity() * sizeof(float); }
    Result<std::vector<float>> evaluate(TokenId, std::vector<float> &keys,
                                        std::vector<float> &values, ComputeImplementation &) const;
    Result<std::vector<float>> uncached(std::span<const TokenId>, ComputeImplementation &) const;

  private:
    ModelDefinition definition_;
    std::vector<float> weights_;
};
struct SamplingOptions {
    std::uint64_t seed{1};
    float temperature{0.0F};
    float repetition_penalty{1.0F};
    std::size_t top_k{0};
    // The only qualified grammar is a finite, strict UTF-8 literal then EOS.
    std::string literal_grammar;
};
struct NativeOptions {
    std::size_t max_output_tokens{32};
    std::size_t max_input_bytes{255};
    SamplingOptions sampling;
    std::vector<std::string> stop_sequences;
    bool injection_slot{false};
    bool allow_partial_output{false};
};
enum class NativeStage { prefill, decode, completing, stopped, failed, released };
struct NativeExecutionState {
    NativePins pins;
    NativeStage stage{NativeStage::prefill};
    std::uint64_t version{0}, suspension_generation{0};
    std::size_t input_progress{0}, output_progress{0};
    bool valid{true}, paused{false};
};
struct CausalTextState {
    std::vector<TokenId> input, evaluated, emitted;
    std::vector<float> keys, values, logits;
    std::optional<TokenId> pending;
    std::array<std::uint32_t, 258> penalty{};
    std::uint64_t rng{1}, rng_draws{0};
    std::size_t grammar_position{0};
    Utf8Decoder decoder;
    std::string stop_tail, output;
    std::vector<std::uint64_t> injection_commands;
    bool stop_matched{false};
    bool operator==(const CausalTextState &) const = default;
};
struct SegmentReceipt {
    std::uint64_t state_version{0};
    std::size_t evaluated_tokens{0};
    std::optional<TokenId> token;
    std::string text;
    bool quiescent{false}, state_valid{false}, complete{false};
};
struct NativeReleaseReceipt {
    std::uint64_t state_version{0};
    std::size_t released_state_bytes{0};
    bool quiescent{false};
};
// Single worker-owned mutable state. Each call is bounded and returns at a safe point.
class NativeSession {
  public:
    static Result<std::unique_ptr<NativeSession>>
    create(std::shared_ptr<const TinyModel>, std::unique_ptr<ComputeImplementation>,
           const ProcessorDefinition &, const TokenizerDefinition &, std::string_view prompt,
           NativeOptions = {});
    Result<SegmentReceipt> step(std::size_t max_prefill_tokens = 8);
    Result<std::uint64_t> pause();
    Result<void> resume(const NativePins &, std::uint64_t suspension_generation);
    Result<void> inject(std::string_view utf8, std::uint64_t command_id);
    Result<void> request_stop(bool graceful = false);
    Result<NativeReleaseReceipt> release();
    Result<void> unsupported_control(std::string_view) const;
    const NativeExecutionState &state() const noexcept { return state_; }
    const CausalTextState &causal_state() const noexcept { return causal_; }
    const NativeProfile &profile() const noexcept { return profile_; }
    std::size_t retained_bytes() const noexcept;
    std::size_t reserved_state_bytes() const noexcept { return 131072; }
    std::shared_ptr<const TinyModel> model() const noexcept { return model_; }

  private:
    NativeSession() = default;
    Result<void> evaluate_one(TokenId);
    Result<TokenId> sample();
    std::shared_ptr<const TinyModel> model_;
    std::unique_ptr<ComputeImplementation> compute_;
    TokenizerDefinition tokenizer_;
    NativeOptions options_;
    NativeProfile profile_;
    NativeExecutionState state_;
    CausalTextState causal_;
};
} // namespace flamoris::runtime
