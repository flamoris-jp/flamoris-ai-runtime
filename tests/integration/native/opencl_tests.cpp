#include "flamoris/runtime/native.hpp"
#include <catch_amalgamated.hpp>
#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace flamoris::runtime;
namespace {
constexpr auto checksum = "5cd5a0bacb4cce1efd801fe2a4d7c1347467538ef6e393b37d955d2499ef74e2";
void compare(std::span<const float> cpu, std::span<const float> opencl) {
    REQUIRE(cpu.size() == opencl.size());
    for (std::size_t i = 0; i < cpu.size(); ++i)
        REQUIRE(std::abs(cpu[i] - opencl[i]) <= 2.0e-5F + 2.0e-5F * std::abs(cpu[i]));
}
std::unique_ptr<ComputeImplementation> device() {
    const auto index_text = std::getenv("FLAMORIS_OPENCL_DEVICE");
    // Explicit opt-in test executable selects index 0 unless the caller overrides it.
    const auto index = index_text ? static_cast<std::size_t>(std::stoul(index_text)) : 0;
    auto compute = make_opencl_compute(index, 65536);
    REQUIRE(compute); // Missing device fails qualification; this lane never skips or passes a fake.
    std::cout << "Qualified candidate: " << compute.value()->device().identity << '\n';
    return std::move(compute).value();
}
} // namespace
TEST_CASE("B-OPENCL01 real device operations and cached uncached causal parity",
          "[native][opencl]") {
    auto devices = enumerate_opencl_devices();
    REQUIRE(devices);
    REQUIRE_FALSE(devices.value().empty());
    auto cpu = make_cpu_compute(), opencl = device();
    for (std::size_t positions : {1U, 7U, 64U, 256U}) {
        std::vector<float> query(8), keys(positions * 8), values(positions * 8);
        for (std::size_t d = 0; d < query.size(); ++d)
            query[d] = static_cast<float>(d) / 17.0F;
        for (std::size_t i = 0; i < keys.size(); ++i) {
            keys[i] = std::sin(static_cast<float>(i));
            values[i] = std::cos(static_cast<float>(i));
        }
        auto reference = cpu->attention(query, keys, values, positions, 8);
        auto actual = opencl->attention(query, keys, values, positions, 8);
        REQUIRE(reference);
        REQUIRE(actual);
        compare(reference.value(), actual.value());
    }
    auto weights = TinyModel::load(std::filesystem::path(FLAMORIS_SOURCE_DIR) /
                                       "fixtures/native/tiny-causal-v1.bin",
                                   checksum);
    REQUIRE(weights);
    auto ids = tokenize("日本語👩‍💻", {}, 128).value();
    ids.insert(ids.begin(), 256);
    std::vector<float> ck, cv, ok, ov;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        auto c = weights.value()->evaluate(ids[i], ck, cv, *cpu);
        auto o = weights.value()->evaluate(ids[i], ok, ov, *opencl);
        auto full = weights.value()->uncached(std::span<const TokenId>(ids).first(i + 1), *opencl);
        REQUIRE(c);
        REQUIRE(o);
        REQUIRE(full);
        compare(c.value(), o.value());
        compare(o.value(), full.value());
        compare(ck, ok);
        compare(cv, ov);
    }
    REQUIRE(opencl->receipt().completed_operations > 0);
    REQUIRE(opencl->receipt().peak_device_bytes > 0);
    REQUIRE(opencl->receipt().peak_device_bytes <= 65536);
    REQUIRE(opencl->receipt().live_device_bytes == 0);
    REQUIRE(opencl->receipt().quiescent);
}
TEST_CASE("B-OPENCL01 same tokenizer IDs full session pause stop and repeated release",
          "[native][opencl]") {
    auto weights = TinyModel::load(std::filesystem::path(FLAMORIS_SOURCE_DIR) /
                                       "fixtures/native/tiny-causal-v1.bin",
                                   checksum);
    REQUIRE(weights);
    for (int iteration = 0; iteration < 3; ++iteration) {
        NativeOptions options;
        options.max_output_tokens = 24;
        auto cpu =
            NativeSession::create(weights.value(), make_cpu_compute(), {}, {}, "日本語😀", options);
        auto cl = NativeSession::create(weights.value(), device(), {}, {}, "日本語😀", options);
        REQUIRE(cpu);
        REQUIRE(cl);
        REQUIRE(cpu.value()->causal_state().input == cl.value()->causal_state().input);
        REQUIRE(cpu.value()->state().pins.processor == cl.value()->state().pins.processor);
        REQUIRE(cpu.value()->state().pins.tokenizer == cl.value()->state().pins.tokenizer);
        REQUIRE(cpu.value()->state().pins.compute != cl.value()->state().pins.compute);
        for (int segment = 0; segment < 12; ++segment) {
            auto c = cpu.value()->step(4), o = cl.value()->step(4);
            REQUIRE(c);
            REQUIRE(o);
            REQUIRE(c.value().token == o.value().token);
            REQUIRE(c.value().text == o.value().text);
            compare(cpu.value()->causal_state().logits, cl.value()->causal_state().logits);
            if (o.value().complete)
                break;
            auto paused = cl.value()->pause();
            REQUIRE(paused);
            REQUIRE_FALSE(cl.value()->step());
            REQUIRE(cl.value()->resume(cl.value()->state().pins, paused.value()));
        }
        REQUIRE(cl.value()->request_stop());
        REQUIRE_FALSE(cl.value()->step());
        auto released = cl.value()->release();
        REQUIRE(released);
        REQUIRE(released.value().quiescent);
        REQUIRE(cpu.value()->request_stop());
        REQUIRE(cpu.value()->release());
    }
    auto bounded = make_opencl_compute(0, 1);
    REQUIRE(bounded);
    REQUIRE_FALSE(bounded.value()->matvec(std::vector<float>{1}, 1, 1, std::vector<float>{1}));
    REQUIRE(bounded.value()->receipt().live_device_bytes == 0);
}
