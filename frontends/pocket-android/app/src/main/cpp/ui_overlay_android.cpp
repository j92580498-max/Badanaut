#include "ui_overlay.h"
#include "cpu.h"

namespace overlay {

void init(SDL_Window*) {}
void shutdown() {}
bool active() { return false; }
bool handle_event(SDL_Event&) { return false; }
void draw(SDL_Window*) {}
void pump(Cpu&) {}

}
