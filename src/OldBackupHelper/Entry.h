#pragma once

#include "ll/api/mod/NativeMod.h"
#include <SimpleIni.h>

namespace backup_helper {

std::filesystem::path getConfigPath();
CSimpleIniA&          getConfig();
ll::io::Logger&       getLogger();

class BackupHelper {

public:
    static BackupHelper& getInstance();

    BackupHelper() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();

    bool enable();

    bool disable();

    // bool unload();

private:
    ll::mod::NativeMod& mSelf;
};

} // namespace backup_helper
