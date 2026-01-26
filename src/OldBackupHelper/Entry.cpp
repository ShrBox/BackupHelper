#include "Entry.h"

#include "Backup.h"
#include "BackupCommand.h"
#include "Interval.h"
#include "ll/api/Expected.h"
#include "ll/api/i18n/I18n.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/utils/ErrorUtils.h"
#include "mc/server/PropertiesSettings.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/StopCommand.h"


#include <filesystem>
#include <magic_enum.hpp>
#include <memory>

using ll::i18n_literals::operator""_tr;

namespace backup_helper {

CSimpleIniA ini;

std::string_view defaultIni = R"([Main]
; 备份存档保存的最长时间，单位：天 | Maximum backup storage time in days
MaxStorageTime=7
; 备份文件夹位置（相对于服务器根目录） | Backup folder location (relative to server root)
BackupPath=.\backup
; 备份文件压缩等级，可选等级有0,1,3,5,7,9 | Backup compression level (0=store only, 1=fastest, 9=best)
; 默认为0，即仅打包 | Default is 0 (no compression, just archive)
Compress=0
; 等待压缩的最长时间，单位：秒，如果为0则无限等待 | Maximum wait time for compression in seconds (0=unlimited)
MaxWaitForZip=1800
; 定期自动备份间隔时间（单位：小时），设为0则禁用自动备份 | Automatic backup interval in hours (0=disabled)
; 注意：服务器重启不会影响定时器 | Note: Server restart does not affect the timer
BackupInterval=24
CommandPermissionLevel=1
ArchiveFormat=7z
)";

std::filesystem::path getConfigPath() { return BackupHelper::getInstance().getSelf().getModDir() / "config.ini"; }
CSimpleIniA&          getConfig() { return ini; }
ll::io::Logger&       getLogger() { return BackupHelper::getInstance().getSelf().getLogger(); }

bool Raw_IniOpen(std::filesystem::path const& path, const std::string_view defContent) {
    if (!std::filesystem::exists(path)) {
        // 创建新的
        std::filesystem::create_directories(std::filesystem::path{path}.remove_filename().u8string());

        std::ofstream iniFile(path);
        if (iniFile.is_open() && !defContent.empty()) iniFile << defContent;
        iniFile.close();
    }

    // 已存在
    getConfig().SetUnicode(true);
    auto res = getConfig().LoadFile(path.c_str());
    if (res < 0) {
        getLogger().error("Failed to open configuration file!"_tr());
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
    Raw_IniOpen(getConfigPath(), defaultIni);
    auto& instance = ll::i18n::getInstance();
    auto  result   = instance.load(getSelf().getLangDir());
    if (!result) {
        ll::error_utils::printCurrentException(getSelf().getLogger());
        return false;
    }
    getSelf().getLogger().info("BackupHelper loaded! Author: yqs112358, ported by: ShrBox"_tr());
    return true;
}

bool BackupHelper::enable() {
    RegisterCommand();
    StartInterval();
    return true;
}

bool BackupHelper::disable() {
    StopInterval();
    return true;
}

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
        getLogger().error(
            "Don't execute stop command when backup"_tr()
        );
        return;
    }
    origin(commandOrigin, output);
}

} // namespace backup_helper

LL_REGISTER_MOD(backup_helper::BackupHelper, backup_helper::BackupHelper::getInstance());
