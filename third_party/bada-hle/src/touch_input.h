#pragma once
#include <SDL2/SDL.h>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <tuple>

namespace input {
enum class TouchKind { pressed, moved, released };
struct TouchEvent {
    TouchKind kind;
    uint32_t point_id;
    int x, y;
};

// Follow touchHLE's SDL finger-ID map and absolute-coordinate conversion.
// Bada delivers each contact separately (CCEGLView_bada.cpp), with the same
// GetPointId value from press through release. Mouse input is a separate key.
class TouchInput {
  public:
    using Sink = std::function<void(const TouchEvent&)>;

    bool handle(const SDL_Event& event, int width, int height, bool multipoint, const Sink& sink) {
        if (event.type == SDL_FINGERDOWN || event.type == SDL_FINGERMOTION || event.type == SDL_FINGERUP) {
            if (event.tfinger.touchId == SDL_MOUSE_TOUCHID || width <= 0 || height <= 0) return true;
            const auto kind = event.type == SDL_FINGERDOWN ? TouchKind::pressed :
                              event.type == SDL_FINGERUP ? TouchKind::released : TouchKind::moved;
            update({true, event.tfinger.touchId, event.tfinger.fingerId}, kind,
                   static_cast<int>(event.tfinger.x * width), static_cast<int>(event.tfinger.y * height),
                   multipoint, sink);
            return true;
        }
        if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) {
            if (event.button.which == SDL_TOUCH_MOUSEID) return true;
            if (event.button.button != SDL_BUTTON_LEFT) return false;
            update({false, 0, 0}, event.type == SDL_MOUSEBUTTONDOWN ? TouchKind::pressed : TouchKind::released,
                   event.button.x, event.button.y, multipoint, sink);
            return true;
        }
        if (event.type == SDL_MOUSEMOTION) {
            if (event.motion.which == SDL_TOUCH_MOUSEID) return true;
            update({false, 0, 0}, TouchKind::moved, event.motion.x, event.motion.y, multipoint, sink);
            return true;
        }
        return false;
    }

    void cancel(const Sink& sink) {
        while (!contacts_.empty()) {
            const Contact contact = contacts_.begin()->second;
            contacts_.erase(contacts_.begin());
            if (contact.delivered) sink({TouchKind::released, contact.point_id, contact.x, contact.y});
        }
    }

  private:
    using Key = std::tuple<bool, SDL_TouchID, SDL_FingerID>;
    struct Contact { uint32_t point_id; int x, y; bool delivered; };
    std::map<Key, Contact> contacts_;

    void update(const Key& key, TouchKind kind, int x, int y, bool multipoint, const Sink& sink) {
        auto found = contacts_.find(key);
        if (kind == TouchKind::pressed) {
            if (found != contacts_.end()) return;
            uint32_t id = 0;
            while (std::any_of(contacts_.begin(), contacts_.end(), [id](const auto& entry) {
                return entry.second.point_id == id;
            })) ++id;
            const bool delivered = multipoint || std::none_of(contacts_.begin(), contacts_.end(), [](const auto& entry) {
                return entry.second.delivered;
            });
            found = contacts_.emplace(key, Contact{id, x, y, delivered}).first;
        } else if (found == contacts_.end()) {
            return; // a cancelled/unknown finger cannot move another contact
        }
        found->second.x = x;
        found->second.y = y;
        const Contact contact = found->second;
        if (kind == TouchKind::released) contacts_.erase(found);
        if (contact.delivered) sink({kind, contact.point_id, x, y});
    }
};
} // namespace input
