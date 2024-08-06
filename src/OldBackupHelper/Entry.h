#pragma once

#include "ll/api/mod/NativeMod.h"
#include <SimpleIni.h>

namespace backup_helper {
std::filesystem::path getConfigPath();
CSimpleIniA&          getConfig();
class BackupHelper {

public:
    static BackupHelper& getInstance();

    BackupHelper(ll::mod::NativeMod& self) : mSelf(self) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();

    bool enable();

    bool disable();

    // bool unload();

private:
    ll::mod::NativeMod& mSelf;
};

} // namespace backup_helper
