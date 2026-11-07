#pragma once
#include <functional>
#include <string>

// Start a localhost-only TCP control server on `port` in a detached background
// thread. For each newline-terminated line a client sends, calls exec(line) and
// writes the returned reply + "\n" back to the client. exec is expected to
// marshal the command to the main thread and block for the reply.
//
// Drive it with the same line protocol as stdin (screenshot/touch/gesture/args/
// peek/poke/call/quit). Bound to 127.0.0.1 only -- it can read/write guest
// memory and invoke guest code, so never expose it off-host.
void start_control_server(int port, std::function<std::string(const std::string&)> exec);
