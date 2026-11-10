#pragma once
#include "performance.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

struct SDL_Window;

// One producer (the emulation thread), one GL-context owner. Commands contain
// host values only. Pointer calls synchronize unless the caller explicitly
// copies their data or guarantees that the pointer is a VBO byte offset.
namespace render {
using Ticket = uint64_t;
bool start(SDL_Window* window, void* context);
void stop();
struct Session { ~Session() { stop(); } };
bool active();
bool on_thread();
void context_changed(void* context); // called by the context owner
Ticket submitted();
bool ready(Ticket ticket);
void wait(Ticket ticket);
void flush();
performance::Work driver_work(const char* name);

using Action = void (*)(void*);
Ticket submit(const void* source, void (*copy)(void*, const void*), Action run, Action destroy);
constexpr size_t command_bytes = 256;

template<class F>
Ticket post(F function) {
    if (!active() || on_thread()) { function(); return 0; }
    static_assert(sizeof(F) <= command_bytes, "render command exceeds the fixed queue slot");
    static_assert(alignof(F) <= alignof(std::max_align_t), "unsupported render command alignment");
    return submit(&function,
        [](void* target, const void* source) { new (target) F(*static_cast<const F*>(source)); },
        [](void* target) { (*static_cast<F*>(target))(); },
        [](void* target) { static_cast<F*>(target)->~F(); });
}

template<class F>
auto sync(F function) -> decltype(function()) {
    using Result = decltype(function());
    if (!active() || on_thread()) return function();
    if constexpr (std::is_void_v<Result>) {
        wait(post(function));
    } else {
        Result result{};
        wait(post([&] { result = function(); }));
        return result;
    }
}

template<class F, class... Args>
auto invoke(performance::Work work, F function, Args... args) -> std::invoke_result_t<F, Args...> {
    using Result = std::invoke_result_t<F, Args...>;
    if constexpr (std::is_void_v<Result> && !(std::is_pointer_v<Args> || ...)) {
        post([=] { performance::Scope scope(work); function(args...); });
    } else {
        return sync([=]() -> Result { performance::Scope scope(work); return function(args...); });
    }
}

template<class F, class... Args>
void offset_call(performance::Work work, F function, Args... args) {
    post([=] { performance::Scope scope(work); function(args...); });
}

template<size_t Count, class F, class T, class... Prefix>
void array_call(performance::Work work, F function, const T* data, Prefix... prefix) {
    std::array<T, Count> values;
    for (size_t i = 0; i < Count; ++i) values[i] = data[i];
    post([=] { performance::Scope scope(work); function(prefix..., values.data()); });
}

struct CopiedBytes { const uint8_t* data; size_t slot; };
CopiedBytes copy_bytes(const void* source, size_t bytes);
void release_bytes(size_t slot);
template<class F>
void data_call(performance::Work work, const void* data, size_t bytes, F function) {
    if (!active() || on_thread() || !data || !bytes || bytes > 64 * 1024 * 1024) {
        sync([=] { performance::Scope scope(work); function(data); });
        return;
    }
    const auto copy = copy_bytes(data, bytes);
    post([=] {
        performance::Scope scope(work);
        function(copy.data);
        release_bytes(copy.slot);
    });
}

template<class F>
class Function {
  public:
    Function(F function, performance::Work work) : function_(function), work_(work) {}
    template<class... Args>
    auto operator()(Args... args) const -> std::invoke_result_t<F, Args...> {
        return invoke(work_, function_, args...);
    }
    explicit operator bool() const { return function_ != nullptr; }
    F pointer() const { return function_; }
    performance::Work work() const { return work_; }
  private:
    F function_;
    performance::Work work_;
};
} // namespace render
