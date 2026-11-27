#pragma once

#include "engine/multiroom_engine.h"

namespace miplay::sidecar {
std::string handle_request(MultiRoomEngine& engine, const std::string& text, bool reject_self = false);
}
