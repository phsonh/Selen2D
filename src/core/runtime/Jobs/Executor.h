#pragma once
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>

namespace core::runtime::Jobs {
namespace detail { struct TaskState; struct ExecutorCore; struct BudgetState; }
enum class State { Queued, Running, Succeeded, Failed, Cancelled };
class Cancelled final : public std::exception {
public:
    const char* what() const noexcept override { return "background task cancelled"; }
};
class StopToken {
public:
    bool IsStopRequested() const noexcept;
    void Check() const;
private:
    friend struct detail::ExecutorCore;
    std::shared_ptr<detail::TaskState> state;
};
class Ticket {
public:
    State GetState() const;
    bool IsFinished() const;
    bool Cancel();
    std::exception_ptr GetError() const;
    void CheckResult() const;
private:
    friend class Executor;
    std::shared_ptr<detail::TaskState> state;
};
struct Config { std::size_t workers{2}, capacity{64}; };
struct Stats { std::size_t queued{}, running{}, submitted{}, completed{}, cancelled{}; };
class Executor {
public:
    explicit Executor(Config = {});
    ~Executor();
    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;
    // Queue + running are bounded. Never executes on the submitting thread;
    // full queue returns nullopt immediately. Tasks must not call Shutdown.
    std::optional<Ticket> TrySubmit(std::function<void(const StopToken&)>);
    Stats GetStats() const;
    // Owner-thread serialized with destruction (no concurrent Shutdown calls).
    // Close, cancel, wake, then join; tasks must cooperate with cancellation.
    void Shutdown();
private:
    std::unique_ptr<detail::ExecutorCore> core;
};
class Reservation {
public:
    Reservation() = default;
    ~Reservation();
    Reservation(Reservation&&) noexcept;
    Reservation& operator=(Reservation&&) noexcept;
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    std::size_t Size() const noexcept { return bytes; }
    void Shrink(std::size_t);
private:
    friend class MemoryBudget;
    std::shared_ptr<detail::BudgetState> state;
    std::size_t bytes{};
};
struct MemoryStats { std::size_t limit{}, used{}, peak{}, waiting{}; };
class MemoryBudget {
public:
    explicit MemoryBudget(std::size_t bytes);
    // Worker-only admission. Waits without holding partial credit; oversized
    // requests fail immediately. Cancellation is checked during bounded waits.
    Reservation Acquire(std::size_t, const StopToken&);
    // Resident assets must not wait for other resident assets to unload while
    // occupying every worker. Nullopt means insufficient currently-free credit.
    // Oversized requests and cancellation throw; inspection/admission are atomic.
    std::optional<Reservation> TryAcquire(std::size_t, const StopToken&);
    MemoryStats GetStats() const;
private:
    std::shared_ptr<detail::BudgetState> state;
};
}
