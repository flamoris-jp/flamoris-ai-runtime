#include "flamoris/runtime/control_mailbox.hpp"
#include <catch_amalgamated.hpp>
#include <array>
#include <atomic>
#include <barrier>
#include <thread>
#include <vector>

using namespace flamoris::runtime;
namespace {
AdmissionCorrelation admission(std::uint64_t operation=1) {
    return {{1,2},PendingSubmissionId(operation),OperationGeneration(1)};
}
ControlMessage message(CallbackCorrelation id, std::uint64_t handle=1) {
    return {ControlMessageKind::validation_ready,std::move(id),OperationId(handle),std::nullopt};
}
}
TEST_CASE("B-CALL01 reserved completion survives full normal ingress", "[mailbox][B-CALL01]") {
    auto made=ControlMailbox::create(1,1); REQUIRE(made);
    auto& box=*made.value();
    REQUIRE(box.post(message(admission(2))));
    REQUIRE_FALSE(box.post(message(admission(3))));
    auto endpoint=box.reserve(admission()); REQUIRE(endpoint);
    REQUIRE_FALSE(box.reserve(admission(4)));
    REQUIRE(endpoint.value().publish(message(admission()))==DeliveryStatus::delivered);
    REQUIRE(endpoint.value().publish(message(admission()))==DeliveryStatus::duplicate);
    REQUIRE(endpoint.value().publish(message(admission(),2))==DeliveryStatus::stale);
    REQUIRE(box.snapshot().ready_completions==1);
    auto completed=box.take(); REQUIRE(completed);
    REQUIRE(completed->correlation==CallbackCorrelation(admission()));
    REQUIRE(box.take()->correlation==CallbackCorrelation(admission(2)));
    REQUIRE_FALSE(box.take());
    REQUIRE(endpoint.value().publish(message(admission()))==DeliveryStatus::stale);
    auto replacement=box.reserve(admission()); REQUIRE(replacement);
    REQUIRE(replacement.value().slot().generation!=endpoint.value().slot().generation);
    REQUIRE(endpoint.value().publish(message(admission()))==DeliveryStatus::stale);
    REQUIRE(replacement.value().publish(message(admission()))==DeliveryStatus::delivered);
}
TEST_CASE("B-DRAIN01 endpoints outlive caller disconnect but not mailbox shutdown", "[mailbox][B-DRAIN01]") {
    CompletionEndpoint late;
    {
        auto made=ControlMailbox::create(2,2); REQUIRE(made);
        auto& box=*made.value();
        auto endpoint=box.reserve(admission()); REQUIRE(endpoint); late=endpoint.value();
        box.close_normal_ingress(); box.close_reservations();
        REQUIRE_FALSE(box.post(message(admission())));
        REQUIRE_FALSE(box.reserve(admission(2)));
        REQUIRE_FALSE(box.close_after_drain());
        REQUIRE(late.publish(message(admission()))==DeliveryStatus::delivered);
        REQUIRE_FALSE(box.withdraw_unstarted(late.slot()));
        REQUIRE(box.take()); REQUIRE(box.close_after_drain());
        REQUIRE(late.publish(message(admission()))==DeliveryStatus::closed);
        REQUIRE_FALSE(box.wait_take());
    }
    REQUIRE(late.publish(message(admission()))==DeliveryStatus::closed);
}
TEST_CASE("B-CALL01 admission lifecycle and cleanup correlations cannot alias", "[mailbox][B-CALL01]") {
    auto made=ControlMailbox::create(1,2); REQUIRE(made);
    auto& box=*made.value();
    auto endpoint=box.reserve(admission()); REQUIRE(endpoint);
    LifecycleCorrelation wrong{{{{1,2},1},1},AttemptId(1),OperationId(1),DispatchGeneration(1),SuspensionGeneration(0)};
    REQUIRE(endpoint.value().publish(message(wrong))==DeliveryStatus::stale);
    REQUIRE(box.withdraw_unstarted(endpoint.value().slot()));
    REQUIRE_FALSE(box.withdraw_unstarted(endpoint.value().slot()));
    REQUIRE(endpoint.value().publish(message(admission()))==DeliveryStatus::stale);
    REQUIRE_FALSE(box.reserve(AdmissionCorrelation{}));
}
TEST_CASE("B-CALL01 real producer race publishes exactly one immutable completion", "[mailbox][threads][B-CALL01]") {
    auto made=ControlMailbox::create(2,1); REQUIRE(made);
    auto& box=*made.value();
    auto endpoint=box.reserve(admission()); REQUIRE(endpoint);
    constexpr std::size_t count=8;
    std::barrier ready(static_cast<std::ptrdiff_t>(count+1));
    std::array<DeliveryStatus,count> outcomes{};
    std::vector<std::jthread> workers;
    for (std::size_t i=0;i<count;++i) workers.emplace_back([&,i,ep=endpoint.value()] {
        ready.arrive_and_wait(); outcomes[i]=ep.publish(message(admission()));
    });
    ready.arrive_and_wait();
    workers.clear(); // join outside inbox lock and any controller turn
    std::size_t delivered=0,duplicates=0;
    for (const auto outcome:outcomes) { delivered+=outcome==DeliveryStatus::delivered; duplicates+=outcome==DeliveryStatus::duplicate; }
    REQUIRE(delivered==1); REQUIRE(duplicates==count-1);
    REQUIRE(box.take()); REQUIRE_FALSE(box.take());
}
TEST_CASE("B-DRAIN01 condition-variable consumer closes without real sleep", "[mailbox][threads][B-DRAIN01]") {
    auto made=ControlMailbox::create(1,1); REQUIRE(made);
    auto& box=*made.value();
    std::barrier ready(2);
    bool empty=false;
    std::jthread consumer([&] { ready.arrive_and_wait(); empty=!box.wait_take(); });
    ready.arrive_and_wait();
    box.close_normal_ingress(); box.close_reservations(); REQUIRE(box.close_after_drain());
    consumer.join(); REQUIRE(empty);
}
