#include "Entry.h"

#include "Backup.h"
#include "BackupCommand.h"
#include "ll/api/Expected.h"
#include "ll/api/i18n/I18n.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/Bedrock.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/PropertiesSettings.h"
#include "mc/server/commands/StopCommand.h"
#include "ll/api/utils/ErrorUtils.h"

#include <filesystem>
#include <magic_enum.hpp>
#include <memory>

using ll::i18n_literals::operator""_tr;

namespace backup_helper {

CSimpleIniA ini;

std::filesystem::path getConfigPath() { return BackupHelper::getInstance().getSelf().getModDir() / "config.ini"; }
CSimpleIniA&          getConfig() { return ini; }

bool Raw_IniOpen(std::filesystem::path const& path, const std::string& defContent) {
    if (!std::filesystem::exists(path)) {
        // 创建新的
        std::filesystem::create_directories(std::filesystem::path{path}.remove_filename().u8string());

        std::ofstream iniFile(path);
        if (iniFile.is_open() && defContent != "") iniFile << defContent;
        iniFile.close();
    }

    // 已存在
    backup_helper::getConfig().SetUnicode(true);
    auto res = backup_helper::getConfig().LoadFile(path.c_str());
    if (res < 0) {
        backup_helper::BackupHelper::getInstance().getSelf().getLogger().error("Failed to open configuration file!"_tr()
        );
        return false;
    } else {
        return true;
    }
}

BackupHelper& BackupHelper::getInstance() {
    static BackupHelper instance;
    return instance;
}

bool BackupHelper::load() {
    Raw_IniOpen(getConfigPath(), "");
    auto& instance = ll::i18n::getInstance();
    auto result = instance.load(getSelf().getLangDir());
    if (!result) {
        ll::error_utils::printCurrentException(getSelf().getLogger());
        return false;
    }
    getSelf().getLogger().info("BackupHelper loaded! Author: yqs112358, ported by: ShrBox"_tr());
    return true;
}

bool BackupHelper::enable() {
    RegisterCommand();
    return true;
}

bool BackupHelper::disable() { return true; }

// 存档开始加载前替换存档文件
LL_AUTO_TYPE_INSTANCE_HOOK(
    PropertiesHook,
    ll::memory::HookPriority::Normal,
    PropertiesSettings,
    &PropertiesSettings::$ctor,
    void*,
    std::string const& filename
) {
    RecoverWorld();
    return origin(filename);
}

LL_AUTO_TYPE_INSTANCE_HOOK(
    StopCommandHook,
    ll::memory::HookPriority::Normal,
    StopCommand,
    &StopCommand::$execute,
    void,
    CommandOrigin const& commandOrigin,
    CommandOutput&       output
) {
    if (GetIsWorking()) {
        backup_helper::BackupHelper::getInstance().getSelf().getLogger().error(
            "Don't execute stop command when backup"_tr()
        );
        return;
    }
    origin(commandOrigin, output);
}

} // namespace backup_helper

LL_REGISTER_MOD(backup_helper::BackupHelper, backup_helper::BackupHelper::getInstance());
