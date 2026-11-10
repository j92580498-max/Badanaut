#include "render_queue.h"
#include <SDL2/SDL.h>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace render {
namespace {
constexpr size_t queue_size = 8192, staging_limit = 64 * 1024 * 1024;
struct Command {
    Action run = nullptr, destroy = nullptr;
    alignas(std::max_align_t) std::array<uint8_t, command_bytes> data;
};
struct Buffer { std::vector<uint8_t> bytes; bool used = false; };
struct Queue {
    SDL_Window* window;
    void* context;
    std::mutex mutex;
    std::condition_variable work, changed;
    std::array<Command, queue_size> commands;
    std::vector<std::unique_ptr<Buffer>> buffers;
    size_t buffer_bytes = 0;
    Ticket head = 0, tail = 0;
    bool initialized = false, bound = false, stopping = false;
    std::exception_ptr failure;
    std::thread worker;
    Queue(SDL_Window* window, void* context) : window(window), context(context) {}
    void run() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            bound = SDL_GL_MakeCurrent(window, context) == 0;
            initialized = true;
        }
        changed.notify_all();
        if (!bound) return;
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            work.wait(lock, [&] { return stopping || head != tail; });
            if (stopping && head == tail) break;
            Command& command = commands[head % queue_size];
            lock.unlock();
            try { command.run(command.data.data()); }
            catch (...) {
                std::lock_guard<std::mutex> failure_lock(mutex);
                if (!failure) failure = std::current_exception();
            }
            command.destroy(command.data.data());
            lock.lock();
            ++head;
            changed.notify_all();
        }
        lock.unlock();
        SDL_GL_MakeCurrent(window, nullptr);
    }
};
std::unique_ptr<Queue> queue;
std::atomic<bool> enabled{false};
} // namespace

bool active() { return enabled.load(std::memory_order_relaxed); }
bool on_thread() { return queue && std::this_thread::get_id() == queue->worker.get_id(); }
bool start(SDL_Window* window, void* context) {
    if (active()) return true;
    auto next = std::make_unique<Queue>(window, context);
    if (SDL_GL_MakeCurrent(window, nullptr) != 0) return false;
    queue = std::move(next);
    try { queue->worker = std::thread([] { queue->run(); }); }
    catch (...) { queue.reset(); SDL_GL_MakeCurrent(window, context); return false; }
    {
        std::unique_lock<std::mutex> lock(queue->mutex);
        queue->changed.wait(lock, [] { return queue->initialized; });
    }
    if (!queue->bound) {
        queue->worker.join();
        queue.reset();
        SDL_GL_MakeCurrent(window, context);
        return false;
    }
    enabled.store(true, std::memory_order_relaxed);
    puts("[gl] dedicated render thread: bounded command queue, copied uploads");
    return true;
}
void context_changed(void* context) { if (queue && on_thread()) queue->context = context; }
void stop() {
    if (!queue) return;
    // Drain even after an error, then restore context ownership for teardown.
    try { flush(); } catch (...) {}
    {
        std::lock_guard<std::mutex> lock(queue->mutex);
        queue->stopping = true;
    }
    queue->work.notify_all();
    queue->worker.join();
    enabled.store(false, std::memory_order_relaxed);
    SDL_GL_MakeCurrent(queue->window, queue->context);
    queue.reset();
}
Ticket submit(const void* source, void (*copy)(void*, const void*), Action run, Action destroy) {
    std::unique_lock<std::mutex> lock(queue->mutex);
    queue->changed.wait(lock, [] { return queue->tail - queue->head < queue_size; });
    if (queue->failure) std::rethrow_exception(queue->failure);
    Command& command = queue->commands[queue->tail % queue_size];
    copy(command.data.data(), source);
    command.run = run;
    command.destroy = destroy;
    const Ticket ticket = ++queue->tail;
    lock.unlock();
    queue->work.notify_one();
    return ticket;
}
Ticket submitted() {
    if (!active()) return 0;
    std::lock_guard<std::mutex> lock(queue->mutex);
    return queue->tail;
}
bool ready(Ticket ticket) {
    if (!ticket || !active()) return true;
    std::lock_guard<std::mutex> lock(queue->mutex);
    return queue->head >= ticket;
}
void wait(Ticket ticket) {
    if (!ticket || !active() || on_thread()) return;
    std::unique_lock<std::mutex> lock(queue->mutex);
    queue->changed.wait(lock, [&] { return queue->head >= ticket; });
    if (queue->failure) std::rethrow_exception(queue->failure);
}
void flush() { wait(submitted()); }

CopiedBytes copy_bytes(const void* source, size_t bytes) {
    std::unique_lock<std::mutex> lock(queue->mutex);
    for (;;) {
        for (size_t i = 0; i < queue->buffers.size(); ++i) {
            auto& buffer = *queue->buffers[i];
            if (buffer.used || buffer.bytes.size() < bytes) continue;
            buffer.used = true;
            std::memcpy(buffer.bytes.data(), source, bytes);
            return {buffer.bytes.data(), i};
        }
        // Large load-time uploads are allowed individually, while ordinary
        // streaming buffers reuse a bounded pool instead of allocating per draw.
        if (queue->buffer_bytes + bytes <= staging_limit) {
            auto buffer = std::make_unique<Buffer>();
            buffer->bytes.resize(bytes);
            buffer->used = true;
            std::memcpy(buffer->bytes.data(), source, bytes);
            const auto* data = buffer->bytes.data();
            queue->buffer_bytes += bytes;
            queue->buffers.push_back(std::move(buffer));
            return {data, queue->buffers.size() - 1};
        }
        bool reclaimed = false;
        for (auto& buffer : queue->buffers) if (!buffer->used && !buffer->bytes.empty()) {
            queue->buffer_bytes -= buffer->bytes.size();
            std::vector<uint8_t>().swap(buffer->bytes);
            reclaimed = true;
        }
        if (!reclaimed) queue->changed.wait(lock);
    }
}
void release_bytes(size_t slot) {
    std::lock_guard<std::mutex> lock(queue->mutex);
    queue->buffers[slot]->used = false;
    queue->changed.notify_all();
}

performance::Work driver_work(const char* name) {
    using performance::Work;
    switch (performance::classify(name)) {
    case Work::draw: return Work::driver_draw;
    case Work::state: return Work::driver_state;
    case Work::texture: return Work::driver_texture;
    case Work::shader: return Work::driver_shader;
    case Work::query: return Work::driver_query;
    case Work::present: return Work::driver_present;
    default: return Work::driver_other;
    }
}
} // namespace render
