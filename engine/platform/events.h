#pragma once

namespace de::platform {

class Input;
class Window;

// Drains the OS event queue. Call once per rendered frame, before sampling input:
//   - window events update window (close request, resize, minimize, focus)
//   - device events go to input with their timestamps (key repeats are ignored)
//   - losing focus releases every held input, so nothing stays stuck down
//   - gamepads are opened when plugged in and closed when removed
void pump_events(Window& window, Input& input);

} // namespace de::platform
