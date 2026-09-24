#pragma once
#ifdef DEVILUTIONX_DEV_TOOLS
#include <sol/sol.hpp>

namespace devilution {

sol::table LuaDevLevelWarpModule(sol::state_view &lua);

} // namespace devilution
#endif // DEVILUTIONX_DEV_TOOLS
