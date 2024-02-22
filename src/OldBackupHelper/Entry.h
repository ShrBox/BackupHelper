#pragma once

#include <ll/api/plugin/NativePlugin.h>

namespace BackupHelper {

[[nodiscard]] auto getSelfPluginInstance() -> ll::plugin::NativePlugin&;

} // namespace BackupHelper
