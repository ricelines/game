#pragma once

#include "react/react.hpp"

namespace amber::bindings {
std::uint32_t unsigned_arg(lua_State *, int index, bool allow_zero = false);
ModelId model_arg(lua_State *, int index);
PlayerId player_arg(lua_State *, int index);
TermId term_arg(lua_State *, int index, const react::Terms &);
Vec3 vector_arg(lua_State *, int index);
Quat rotation_arg(lua_State *, int index);
} // namespace amber::bindings
