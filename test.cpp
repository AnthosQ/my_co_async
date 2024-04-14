#include <cstdint>
#include <cstdio>
#include <coroutine>
#include <queue>
#include <deque>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <span>
#include <format>

using namespace std::chrono_literals;

template <class A>
concept Awaiter = requires(A a, std::coroutine_handle<> h ) {
    { a.await_ready() };
    { a.await_suspend() };
    { a.await_resume() };
};

template <class T = void> struct NonvoidHelper {
    using Type = T;
};

template <class A>
concept Awaitable = Awaiter<A> || requires(A a) {
    { a.operator co_await() } -> Awaiter;
};

template <class A> struct AwaitableTraits;



struct RepeatAwaiter {
    bool await_ready() const noexcept {
        return false;
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> _coroutine) const noexcept {
        if (_coroutine.done())
            return std::noop_coroutine(); 
        else
            return _coroutine;
    }

    void await_resume() const noexcept {}
};

struct PreviousAwaiter {
    bool await_ready() const noexcept {
        return false;
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> _coroutine) const noexcept {
        if (Previous)
            return Previous;
        else
            return std::noop_coroutine();
    }

    void await_resume() const noexcept {}

    std::coroutine_handle<> Previous;
};

template<class T>
struct Promise {
    auto initial_suspend() noexcept {
        return std::suspend_always();
    }

    auto final_suspend() noexcept {
        return PreviousAwaiter(Previous);
    }

    void unhandled_exception() {
        Exception = std::current_exception();
    }

    auto yield_value(T ret) noexcept {
        new (&Result) T(std::move(ret));
        /* return std::suspend_always(); */
        // suspend_never
        return std::suspend_never();
    }
    /* std::suspend_always yield_value(T ret) noexcept { */
    /*     new (&Result) T(std::move(ret)); */
    /*     return {}; */
    /* } */

    void return_value(T ret) noexcept {
        new (&Result) T(std::move(ret));
    }

    T result() {
        if(Exception) [[unlikely]] {
            std::rethrow_exception(Exception);
        }
        T ret = std::move(Result);
        Result.~T();
        return ret;
    }

    std::coroutine_handle<Promise> get_return_object() {
        return std::coroutine_handle<Promise>::from_promise(*this);
    }

    Promise() noexcept {}
    Promise(Promise &&) = delete;
    ~Promise() noexcept {}
 
    std::coroutine_handle<> Previous{};
    std::exception_ptr Exception{};
    union { T Result; };
};

template<>
struct Promise<void> {
    auto initial_suspend() noexcept {
        return std::suspend_always();
    }

    auto final_suspend() noexcept {
        return PreviousAwaiter(Previous);
    }

    void unhandled_exception() {
        Exception = std::current_exception();
    }

    void return_void() noexcept {}

    void result() {
        if(Exception) [[unlikely]] {
            std::rethrow_exception(Exception);
        }
    }

    std::coroutine_handle<Promise> get_return_object() {
        return std::coroutine_handle<Promise>::from_promise(*this);
    }

    Promise() = default;
    Promise(Promise &&) = delete;
    ~Promise() = default;

    std::coroutine_handle<> Previous{};
    std::exception_ptr Exception{};
};

/* template <class T> */
/* struct coroutine_t :std::coroutine_handle<Promise<T>> { */
/*     using promise_type = Promise<T>; */
/* }; */

template <class T = void>
struct Task {
    using promise_type = Promise<T>;
    using coroutine_t = std::coroutine_handle<promise_type>;
    using coroutine_v = std::coroutine_handle<>;
    // coroutine_handle<> 类型擦除

    struct Awaiter {

        bool await_ready() const noexcept { return false; }

        coroutine_v await_suspend(coroutine_v _coroutine) const noexcept {
            Coroutine.promise().Previous = _coroutine;
            return Coroutine;
        }

        T await_resume() const {
            return Coroutine.promise().result();
        }

        coroutine_t Coroutine;
    };

    auto operator co_await() const noexcept {
        return Awaiter(Coroutine);
    }

    operator coroutine_v() const noexcept {
        return Coroutine;
    }

    Task(coroutine_t _coroutine) noexcept : Coroutine(_coroutine) {}
    Task(Task &&) = delete;
    ~Task() { Coroutine.destroy(); }

    coroutine_t Coroutine;
};

// using epoll to rewrite it?
struct Loop_queue {
    std::deque<std::coroutine_handle<>> ReadyQueue;

    struct TimeEntry {
        std::chrono::system_clock::time_point expireTime;
        std::coroutine_handle<> coroutine;

        bool operator<(TimeEntry const &that) const noexcept {
            return expireTime > that.expireTime;
        }
    };

    std::priority_queue<TimeEntry> TimeHeap;

    void addTask(std::coroutine_handle<> _coroutine) {
        ReadyQueue.push_front(_coroutine);
    }

    void addTimer(std::chrono::system_clock::time_point _expireTime, std::coroutine_handle<> _coroutine) {
        TimeHeap.push({_expireTime, _coroutine});
    }

    void runAll() {
        while (!TimeHeap.empty() || !ReadyQueue.empty()) {
            while (!ReadyQueue.empty()) {
                auto coroutine = ReadyQueue.front();
                ReadyQueue.pop_front();
                coroutine.resume();
            }
            if (!TimeHeap.empty()) {
                auto nowTime = std::chrono::system_clock::now();
                auto timer = std::move(TimeHeap.top());
                if (timer.expireTime < nowTime) {
                    TimeHeap.pop();
                    timer.coroutine.resume();
                } else {
                    std::this_thread::sleep_until(timer.expireTime);
                }
            }
        }
    }

    Loop_queue &operator=(Loop_queue &&) = delete;
};

Loop_queue &getLoop() {
    static Loop_queue loop;
    return loop;
}

struct SleepAwaiter {
    bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> _coroutine) const noexcept {
        getLoop().addTimer(ExpireTime, _coroutine);
    }

    void await_resume() const noexcept {
    }

    std::chrono::system_clock::time_point ExpireTime;
};

Task<void> sleep_until(std::chrono::system_clock::time_point expireTime) {
    co_await SleepAwaiter(expireTime);
    co_return;
}

Task<void> sleep_for(std::chrono::system_clock::duration duration) {
    co_await SleepAwaiter(std::chrono::system_clock::now() + duration);
    co_return;
}

Task<int> sleep1() {
    printf("sleep1 sleep 1s\n");
    co_await sleep_for(1s);
    printf("sleep1 awake\n");
    co_return 1;
}

Task<int> sleep2() {
    printf("sleep2 sleep 2s\n");
    co_await sleep_for(2s);
    printf("sleep2 awake\n");
    co_return 2;
}

Task<uint32_t> fib(uint32_t limit) {
    /* std::cout << "hello\n"; */
    uint32_t a{1};
    uint32_t b{1};
    uint32_t c{};
    co_yield a;
    co_yield b;

    if (limit == 2 || limit == 1)
        co_return 1;
    if (limit < 1)
        throw std::runtime_error("index is wrong");

    for (uint32_t index = 2; index < limit - 1; index++) {
        c = a + b;
        co_yield c;
        printf("%d\n",c);
        a = b;
        b = c;
    }
    c = a + b;
    co_return c;
};

Task<uint32_t> hello() {
    uint32_t index = 10;
    co_return index;
};
/* Task<int> test() { */
/*  */
/* }; */
// ?
Task<> test1() {
    uint32_t limit = co_await hello();
    std::cout << "coroutine hello() done\n";
    auto f = fib(limit);
    f.Coroutine.resume();
    /* while(!f.Coroutine.done()) { */
    /*     f.Coroutine.resume(); */
    /*     printf("%d\n",f.Coroutine.promise().result()); */
    /* } */
    std::cout << "coroutine fib() done\n";
};

int main() {
    /* auto tst = test(); */
    /* auto fib1 = test1(); */
    /* while (!fib1.Coroutine.done()) { */
    /*     fib1.Coroutine.resume(); */
    /* } */
    auto t1 = sleep1();
    auto t2 = sleep2();
    auto tst = test1();
    getLoop().addTask(t1);
    getLoop().addTask(tst);
    getLoop().addTask(t2);
    getLoop().runAll();
    printf("%d\n", t1.Coroutine.promise().result());
    printf("%d\n", t2.Coroutine.promise().result());
    return 0;
}
