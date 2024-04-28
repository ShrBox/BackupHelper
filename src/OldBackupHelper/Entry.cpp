#include "Entry.h"
#include <SimpleIni.h>

#include "Backup.h"
#include "BackupCommand.h"
#include "ConfigFile.h"
#include "ll/api/i18n/I18n.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/plugin/NativePlugin.h"
#include "ll/api/plugin/RegisterHelper.h"
#include "ll/api/service/Bedrock.h"
#include <fmt/format.h>
#include <functional>
#include <ll/api/Config.h>
#include <ll/api/event/EventBus.h>
#include <ll/api/event/command/ExecuteCommandEvent.h>
#include <ll/api/io/FileUtils.h>
#include <ll/api/plugin/NativePlugin.h>
#include <ll/api/plugin/PluginManagerRegistry.h>
#include <mc/server/common/PropertiesSettings.h>
#include <memory>
#include <stdexcept>


CSimpleIniA ini;
using ll::i18n_literals::operator""_tr;

// 存档开始加载前替换存档文件
LL_AUTO_TYPE_INSTANCE_HOOK(
    PropertiesHook,
    ll::memory::HookPriority::Normal,
    PropertiesSettings,
    "??0PropertiesSettings@@QEAA@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z",
    void,
    std::string const& filename
) {
    RecoverWorld();
    origin(filename);
}

bool Raw_IniOpen(const magic_enum::string& path, const std::string& defContent) {
    if (!std::filesystem::exists(path)) {
        // 创建新的
        std::filesystem::create_directories(std::filesystem::path(path).remove_filename().u8string());

        std::ofstream iniFile(path);
        if (iniFile.is_open() && defContent != "") iniFile << defContent;
        iniFile.close();
    }

    // 已存在
    ini.SetUnicode(true);
    auto res = ini.LoadFile(path.c_str());
    if (res < 0) {
        backup_helper::BackupHelper::getInstance().getSelf().getLogger().error("Failed to open configuration file!"_tr()
        );
        return false;
    } else {
        return true;
    }
}

namespace backup_helper {

static std::unique_ptr<BackupHelper> instance;

BackupHelper& BackupHelper::getInstance() { return *instance; }

bool BackupHelper::load() {
    getSelf().getLogger().info("Loading...");
    // Code for loading the plugin goes here.
    Raw_IniOpen(_CONFIG_FILE, "");
    ll::i18n::load("./plugins/BackupHelper/lang/");
    ll::i18n::getInstance()->mDefaultLocaleName = ini.GetValue("Main", "Language", "en_US");
    getSelf().getLogger().info("BackupHelper loaded! Author: yqs112358, ported by: ShrBox"_tr());
    return true;
}

bool BackupHelper::enable() {
    getSelf().getLogger().info("Enabling...");
    // Code for enabling the plugin goes here.
    auto& logger = getSelf().getLogger();
    RegisterCommand();
    ll::event::EventBus::getInstance().emplaceListener<ll::event::ExecutingCommandEvent>(
        [&logger](ll::event::ExecutingCommandEvent& ev) {
            std::string cmd = ev.commandContext().mCommand;
            if (cmd.starts_with("/")) {
                cmd.erase(0, 1);
            }
            if (cmd == "stop" && GetIsWorking()) {
                logger.error("Don't execute stop command when backup"_tr());
            }
        }
    );
    return true;
}

bool BackupHelper::disable() {
    getSelf().getLogger().info("Disabling...");
    // Code for disabling the plugin goes here.
    return true;
}

} // namespace backup_helper

LL_REGISTER_PLUGIN(backup_helper::BackupHelper, backup_helper::instance);
