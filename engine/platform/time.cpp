#include "platform/time.h"

#include <SDL3/SDL_timer.h>

namespace de::platform {

uint64_t time_ns() {
    return SDL_GetTicksNS();
}

void sleep_ns(uint64_t duration_ns) {
    SDL_DelayNS(duration_ns);
}

} // namespace de::platform
