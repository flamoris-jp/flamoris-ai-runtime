#include "../support/manual_resource_ports.hpp"
#include "flamoris/runtime/runtime_residency.hpp"
#include <catch_amalgamated.hpp>
using namespace flamoris::runtime;
using namespace flamoris::runtime::test;
using namespace std::chrono_literals;
namespace {
constexpr auto checksum = "5cd5a0bacb4cce1efd801fe2a4d7c1347467538ef6e393b37d955d2499ef74e2";
ResourceVector ram(std::uint64_t bytes, std::uint64_t slots = 0) {
    ResourceVector v;
    v[ResourceKind::ram] = bytes;
    v[ResourceKind::execution] = slots;
    return v;
}
struct PoolFixture {
    RuntimeInstanceId instance{7, 9};
    ManualHost host;
    ManualCleanupObservation observations;
    ResourceManager resources{instance, host, observations};
    RuntimeResidencyPool pool{resources, 2, 4};
    ModelResidencyKey key{{}, LogicalResourceId{1}, HostEpoch{1}, NativeWorkerGeneration{1}};
    PoolFixture() {
        REQUIRE(resources.observe_envelope({key.resource, instance, key.host_epoch, 1, 1000ns,
                                            ram(1000000, 2), HostArbitration::enforced_generation,
                                            true, true},
                                           0ns));
    }
    std::pair<ResourceTicket, ExecutionLease> start(std::uint64_t id, std::uint64_t bytes,
                                                    std::vector<AllocationIdentity> shared = {}) {
        ResourceRequest request{
            OperationId{id},  {JobId{RunId{instance, id}, 1}, AttemptId{1}, DispatchGeneration{1}},
            key.resource,     key.host_epoch,
            key.worker,       ram(bytes, 1),
            ram(300000, 1),   900ns,
            std::move(shared)};
        auto t = resources.reserve(request, 0ns);
        REQUIRE(t);
        REQUIRE(resources.acquired(
            {t.value(), instance, OperationId{100 + id}, HostOutcome::acknowledged}));
        auto l = resources.commit_dispatch(t.value(), {true, true, true, true}, 0ns);
        REQUIRE(l);
        return {t.value(), l.value()};
    }
    AllocationIdentity materialize(ResourceTicket ticket, std::size_t bytes) {
        auto id = resources.next_allocation_identity(ticket);
        REQUIRE(id);
        REQUIRE(resources.materialized(ticket, id.value(), ram(bytes)));
        return id.value();
    }
    void free_state(AllocationIdentity id, JobId job, std::uint64_t operation) {
        REQUIRE(resources.drop_reference(id, job));
        REQUIRE(resources.request_release(id, OperationId{operation}));
        REQUIRE(resources.release_confirmed({id, OperationId{operation}, true}, 1ns));
    }
};
} // namespace
TEST_CASE("A11 B-ACT04 Runtime residency shares one real model across independent native sessions",
          "[resources][residency][A11][B-ACT04]") {
    PoolFixture f;
    auto [t1, l1] = f.start(1, 25984 + 131072 + 65536);
    REQUIRE(f.pool.begin(f.key, t1.owner, l1, false, 900ns, 0ns));
    REQUIRE(f.pool.availability(f.key, 0ns) == ModelLoadDecision::wait_without_lease);
    auto loaded = TinyModel::load(std::filesystem::path(FLAMORIS_SOURCE_DIR) /
                                      "fixtures/native/tiny-causal-v1.bin",
                                  checksum);
    REQUIRE(loaded);
    auto session1 = NativeSession::create(loaded.value(), make_cpu_compute(), {}, {}, "one");
    REQUIRE(session1);
    const auto model_bytes = loaded.value()->resident_bytes();
    auto model = f.materialize(t1, model_bytes);
    auto state1 = f.materialize(t1, session1.value()->reserved_state_bytes());
    std::array manifest1{model, state1};
    REQUIRE(f.resources.materialization_complete(t1, manifest1, 0ns));
    REQUIRE(f.pool.completed(f.key, t1.owner, std::move(loaded).value(), model, 0ns));
    REQUIRE(f.pool.view(f.key, 0ns).value().model_bytes == model_bytes);
    auto [t2, l2] = f.start(2, 131072 + 65536, {model});
    auto shared = f.pool.attach(f.key, t2.owner.job, ram(300000, 1), 0ns);
    REQUIRE(shared);
    auto session2 =
        NativeSession::create(std::move(shared).value(), make_cpu_compute(), {}, {}, "two");
    REQUIRE(session2);
    auto state2 = f.materialize(t2, session2.value()->reserved_state_bytes());
    std::array manifest2{state2};
    REQUIRE(f.resources.materialization_complete(t2, manifest2, 0ns));
    REQUIRE(f.resources.snapshot(f.key.resource).resident == ram(model_bytes + 2 * 131072));
    REQUIRE(f.resources.allocation(model)->references == 2);
    REQUIRE(session1.value()->request_stop());
    REQUIRE(session1.value()->release());
    f.free_state(state1, t1.owner.job, 11);
    REQUIRE(f.resources.quiesced({l1, ContainmentProof::worker_quiesced, true, true}, 1ns));
    auto first_release = f.pool.release_job(f.key, t1.owner.job, OperationId{12}, 1ns);
    REQUIRE(first_release);
    REQUIRE_FALSE(first_release.value());
    REQUIRE(f.resources.snapshot(f.key.resource).resident == ram(model_bytes + 131072));
    REQUIRE(f.pool.availability(f.key, 1ns) == ModelLoadDecision::resident_available);
    REQUIRE(session2.value()->request_stop());
    REQUIRE(session2.value()->release());
    f.free_state(state2, t2.owner.job, 21);
    REQUIRE(f.resources.quiesced({l2, ContainmentProof::worker_quiesced, true, true}, 2ns));
    REQUIRE(f.pool.release_job(f.key, t2.owner.job, OperationId{22}, 2ns).value());
    REQUIRE(f.resources.snapshot(f.key.resource).resident.empty());
    REQUIRE(f.pool.availability(f.key, 2ns) == ModelLoadDecision::load_owned);
    REQUIRE(f.pool.retire_settled() == 1);
    REQUIRE(f.resources.retire_settled() > 0);
}
TEST_CASE("C06 a last logical model user is not physical release proof",
          "[resources][residency][A29]") {
    PoolFixture f;
    auto [ticket, lease] = f.start(1, 30000);
    REQUIRE(f.pool.begin(f.key, ticket.owner, lease, false, 900ns, 0ns));
    auto model = TinyModel::load(std::filesystem::path(FLAMORIS_SOURCE_DIR) /
                                     "fixtures/native/tiny-causal-v1.bin",
                                 checksum);
    REQUIRE(model);
    auto accidental_holder = model.value();
    const auto bytes = model.value()->resident_bytes();
    auto allocation = f.materialize(ticket, bytes);
    std::array manifest{allocation};
    REQUIRE(f.resources.materialization_complete(ticket, manifest, 0ns));
    REQUIRE(f.pool.completed(f.key, ticket.owner, std::move(model).value(), allocation, 0ns));
    REQUIRE(f.resources.quiesced({lease, ContainmentProof::worker_quiesced, true, true}, 1ns));
    auto released = f.pool.release_job(f.key, ticket.owner.job, OperationId{10}, 1ns);
    REQUIRE(released);
    REQUIRE_FALSE(released.value());
    REQUIRE(f.pool.availability(f.key, 1ns) == ModelLoadDecision::wait_without_lease);
    REQUIRE(f.resources.snapshot(f.key.resource).resident == ram(bytes));
    REQUIRE(f.pool.retire_settled() == 0);
    accidental_holder.reset();
    REQUIRE(f.pool.reconcile(f.key, 2ns).value());
    REQUIRE(f.resources.snapshot(f.key.resource).resident.empty());
}
TEST_CASE("C06 residency loss fences consumption without discarding model ownership",
          "[resources][residency][B-ACT05]") {
    PoolFixture f;
    auto [ticket, lease] = f.start(1, 30000);
    REQUIRE(f.pool.begin(f.key, ticket.owner, lease, false, 900ns, 0ns));
    REQUIRE_FALSE(f.pool.can_pause(f.key));
    auto model = TinyModel::load(std::filesystem::path(FLAMORIS_SOURCE_DIR) /
                                     "fixtures/native/tiny-causal-v1.bin",
                                 checksum);
    REQUIRE(model);
    const auto bytes = model.value()->resident_bytes();
    auto allocation = f.materialize(ticket, bytes);
    std::array manifest{allocation};
    REQUIRE(f.resources.materialization_complete(ticket, manifest, 0ns));
    REQUIRE(f.pool.completed(f.key, ticket.owner, std::move(model).value(), allocation, 0ns));
    f.resources.fence(f.key.resource);
    f.pool.fence(f.key.host_epoch);
    REQUIRE_FALSE(f.pool.view(f.key, 1ns));
    REQUIRE_FALSE(f.pool.attach(f.key, JobId{RunId{f.instance, 2}, 1}, ram(300000, 1), 1ns));
    REQUIRE(f.resources.snapshot(f.key.resource).resident == ram(bytes));
    REQUIRE(f.resources.quiesced({lease, ContainmentProof::worker_quiesced, true, true}, 1ns));
    REQUIRE(f.pool.release_job(f.key, ticket.owner.job, OperationId{10}, 1ns).value());
}
