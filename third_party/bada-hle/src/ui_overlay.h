#pragma once
#include <SDL2/SDL.h>

class Cpu;

// Host-side stand-in for the native bada UI controls the app asks for (see
// hle/ui.h). Nuklear draws them over the guest's GL surface after the frame.
namespace overlay {

void init(SDL_Window* win);
void shutdown();
// True when the overlay consumed the event (a control has focus). The caller
// must not forward those to the guest as touches.
bool handle_event(SDL_Event& e);
// Draw the mocked controls. Called from inside the guest's swap, so it only
// renders -- no GL state left behind, and no guest re-entry.
void draw(SDL_Window* win);
void begin_frame();
// Native keypad sessions can suspend the guest's draws. Keep a desktop text
// control responsive during those frames without duplicating a guest swap.
void present_idle(SDL_Window* win);
// Replay what the user did into the app's listeners. Must run from the frame
// loop, outside any guest call.
void pump(Cpu& cpu);
bool active();

} // namespace overlay
