#include "Executor.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace core::runtime::Jobs {
namespace detail {
struct TaskState {
    mutable std::mutex mutex;
    std::atomic_bool stop{};
    State status{State::Queued};
    std::exception_ptr error;
};
namespace {
thread_local ExecutorCore* currentExecutor{};
bool Terminal(State state) noexcept {
    return state == State::Succeeded || state == State::Failed || state == State::Cancelled;
}
bool RequestStop(TaskState& task) {
    const std::lock_guard lock(task.mutex);
    if (Terminal(task.status)) return false;
    task.stop.store(true, std::memory_order_release);
    return true;
}
}
struct ExecutorCore {
    struct Work {
        std::shared_ptr<TaskState> state;
        std::function<void(const StopToken&)> function;
    };
    std::mutex mutex;
    std::condition_variable wake;
    const std::size_t capacity;
    std::deque<Work> queue;
    std::vector<std::shared_ptr<TaskState>> active;
    std::vector<std::thread> workers;
    Stats stats;
    bool closing{}, joined{};

    explicit ExecutorCore(Config config) : capacity(config.capacity) {
        if (!config.workers || !config.capacity)
            throw std::invalid_argument("executor workers and capacity must be positive");
        active.resize(config.workers);
        workers.reserve(config.workers);
        try {
            for (std::size_t index = 0; index < config.workers; ++index)
                workers.emplace_back([this, index] { Run(index); });
        } catch (...) {
            // A partial thread-construction failure must not leave threads
            // accessing a Core whose constructor is about to unwind.
            Close();
            for (auto& worker : workers) if (worker.joinable()) worker.join();
            throw;
        }
    }
    void Close() {
        {
            const std::lock_guard lock(mutex);
            closing = true;
            for (auto& work : queue) RequestStop(*work.state);
            for (const auto& state : active) if (state) RequestStop(*state);
        }
        wake.notify_all();
    }
    void Shutdown() {
        if (currentExecutor)
            throw std::logic_error("executor Shutdown must not be called from a worker task");
        {
            const std::lock_guard lock(mutex);
            if (joined) return;
        }
        // Owner serializes Shutdown/destruction. TrySubmit/GetStats/Ticket
        // operations may run concurrently, but the thread vector has one owner.
        Close();
        for (auto& worker : workers) if (worker.joinable()) worker.join();
        const std::lock_guard lock(mutex);
        joined = true;
    }
    void Run(std::size_t index) {
        currentExecutor = this;
        for (;;) {
            Work work;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return closing || !queue.empty(); });
                if (queue.empty()) break;
                work.state = std::move(queue.front().state);
                // swap leaves a provably empty function in the queue node;
                // popping it under the lock cannot run a user capture destructor.
                work.function.swap(queue.front().function);
                queue.pop_front();
                active[index] = work.state;
                --stats.queued;
                ++stats.running;
                const std::lock_guard taskLock(work.state->mutex);
                work.state->status = State::Running;
            }
            StopToken token;
            token.state = work.state;
            std::exception_ptr error;
            bool cancelled{};
            try {
                token.Check();
                work.function(token); // No core, task-state or budget lock held.
            } catch (const Cancelled&) { cancelled = true; }
            catch (...) { error = std::current_exception(); }

            // Even queued tasks cancelled at Shutdown release their captures on
            // a worker, outside locks. Ticket only retains the small TaskState.
            // Capacity is not returned until this potentially heavy cleanup ends.
            work.function = {};
            {
                const std::lock_guard lock(mutex);
                const std::lock_guard taskLock(work.state->mutex);
                cancelled = cancelled || work.state->stop.load(std::memory_order_acquire);
                work.state->status = cancelled ? State::Cancelled : error ? State::Failed : State::Succeeded;
                work.state->error = cancelled ? std::exception_ptr{} : std::move(error);
                active[index].reset();
                --stats.running;
                ++stats.completed;
                if (cancelled) ++stats.cancelled;
            }
        }
        currentExecutor = nullptr;
    }
};
struct BudgetState {
    std::mutex mutex;
    std::condition_variable released;
    std::size_t limit{}, used{}, peak{}, waiting{};
    explicit BudgetState(std::size_t bytes) : limit(bytes) {}
};
namespace {
void Release(const std::shared_ptr<BudgetState>& state, std::size_t bytes) noexcept {
    if (!state || !bytes) return;
    {
        const std::lock_guard lock(state->mutex);
        state->used -= bytes;
    }
    state->released.notify_all();
}
}
}

bool StopToken::IsStopRequested() const noexcept {
    return state && state->stop.load(std::memory_order_acquire);
}
void StopToken::Check() const {
    if (IsStopRequested()) throw Cancelled();
}
State Ticket::GetState() const {
    if (!state) throw std::logic_error("invalid background task ticket");
    const std::lock_guard lock(state->mutex);
    return state->status;
}
bool Ticket::IsFinished() const { return detail::Terminal(GetState()); }
bool Ticket::Cancel() {
    if (!state) throw std::logic_error("invalid background task ticket");
    return detail::RequestStop(*state);
}
std::exception_ptr Ticket::GetError() const {
    if (!state) throw std::logic_error("invalid background task ticket");
    const std::lock_guard lock(state->mutex);
    return state->error;
}
void Ticket::CheckResult() const {
    if (!state) throw std::logic_error("invalid background task ticket");
    State status;
    std::exception_ptr error;
    {
        const std::lock_guard lock(state->mutex);
        status = state->status;
        error = state->error;
    }
    if (status == State::Cancelled) throw Cancelled();
    if (status == State::Failed) std::rethrow_exception(error);
    if (status != State::Succeeded) throw std::logic_error("background task is not finished");
}
Executor::Executor(Config config) : core(std::make_unique<detail::ExecutorCore>(config)) {}
Executor::~Executor() {
    // Destruction is an owner-thread operation. Destroying an Executor from a
    // managed worker violates the same contract as calling Shutdown there.
    try { core->Shutdown(); } catch (...) { std::terminate(); }
}
std::optional<Ticket> Executor::TrySubmit(std::function<void(const StopToken&)> function) {
    if (!function) throw std::invalid_argument("background task must have a function");
    std::optional<Ticket> ticket;
    {
        const std::lock_guard lock(core->mutex);
        if (core->closing || core->stats.queued + core->stats.running >= core->capacity) return std::nullopt;
        Ticket candidate;
        candidate.state = std::make_shared<detail::TaskState>();
        // Reserve an empty node before transferring captures. If allocation
        // throws, no populated temporary Work is destroyed under the core lock.
        core->queue.emplace_back();
        core->queue.back().state = candidate.state;
        core->queue.back().function.swap(function);
        ++core->stats.queued;
        ++core->stats.submitted;
        ticket = std::move(candidate);
    }
    core->wake.notify_one();
    return ticket;
}
Stats Executor::GetStats() const {
    const std::lock_guard lock(core->mutex);
    return core->stats;
}
void Executor::Shutdown() { core->Shutdown(); }

Reservation::~Reservation() { detail::Release(state, bytes); }
Reservation::Reservation(Reservation&& other) noexcept
    : state(std::move(other.state)), bytes(std::exchange(other.bytes, 0)) {}
Reservation& Reservation::operator=(Reservation&& other) noexcept {
    if (this != &other) {
        detail::Release(state, bytes);
        state = std::move(other.state);
        bytes = std::exchange(other.bytes, 0);
    }
    return *this;
}
void Reservation::Shrink(std::size_t size) {
    if (size > bytes) throw std::invalid_argument("memory reservation cannot grow");
    const auto release = bytes - size;
    bytes = size;
    detail::Release(state, release);
}
MemoryBudget::MemoryBudget(std::size_t bytes) {
    if (!bytes) throw std::invalid_argument("memory budget must be positive");
    state = std::make_shared<detail::BudgetState>(bytes);
}
Reservation MemoryBudget::Acquire(std::size_t bytes, const StopToken& token) {
    std::unique_lock lock(state->mutex);
    if (bytes > state->limit) throw std::length_error("memory reservation exceeds budget limit");
    struct WaitRegistration {
        detail::BudgetState& budget;
        bool registered{};
        void Block() {
            if (!registered) { ++budget.waiting; registered = true; }
        }
        // Created after the unique_lock, so every return/throw decrements under
        // that same lock before it is released. Polls/spurious wakes count one
        // waiting request, never a new waiter on every condition-variable wait.
        ~WaitRegistration() { if (registered) --budget.waiting; }
    } registration{*state};
    for (;;) {
        token.Check();
        if (bytes <= state->limit - state->used) {
            Reservation reservation;
            reservation.state = state;
            reservation.bytes = bytes;
            state->used += bytes;
            state->peak = std::max(state->peak, state->used);
            return reservation;
        }
        // Tickets need not retain or enumerate every budget. The stop flag is
        // polled during bounded waits, independently of the game/frame thread.
        registration.Block();
        state->released.wait_for(lock, std::chrono::milliseconds(10));
    }
}
std::optional<Reservation> MemoryBudget::TryAcquire(std::size_t bytes, const StopToken& token) {
    const std::lock_guard lock(state->mutex);
    token.Check();
    if (bytes > state->limit) throw std::length_error("memory reservation exceeds budget limit");
    if (bytes > state->limit - state->used) return std::nullopt;
    Reservation reservation;
    reservation.state = state;
    reservation.bytes = bytes;
    state->used += bytes;
    state->peak = std::max(state->peak, state->used);
    return reservation;
}
MemoryStats MemoryBudget::GetStats() const {
    const std::lock_guard lock(state->mutex);
    return {state->limit, state->used, state->peak, state->waiting};
}
}
