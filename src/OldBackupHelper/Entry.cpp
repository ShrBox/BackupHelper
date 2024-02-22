#include "Entry.h"
#include <SimpleIni.h>

#include "Backup.h"
#include "BackupCommand.h"
#include "ConfigFile.h"
#include "ll/api/i18n/I18n.h"
#include "ll/api/memory/Hook.h"
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

namespace BackupHelper {

namespace {

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
        BackupHelper::getSelfPluginInstance().getLogger().error("Failed to open Config File!");
        return false;
    } else {
        return true;
    }
}

std::unique_ptr<std::reference_wrapper<ll::plugin::NativePlugin>>
    selfPluginInstance; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

auto disable(ll::plugin::NativePlugin& /*self*/) -> bool { return true; }

auto enable(ll::plugin::NativePlugin& self) -> bool {
    auto& logger = self.getLogger();
    ll::event::EventBus::getInstance().emplaceListener<ll::event::ExecutingCommandEvent>(
        [&logger](ll::event::ExecutingCommandEvent& ev) {
            std::string cmd = ev.commandContext().mCommand;
            if (cmd.starts_with("/")) {
                cmd.erase(0, 1);
            }
            if (cmd == "stop" && GetIsWorking()) {
                logger.error("Don't execute stop command when backup");
            }
        }
    );
    return true;
}

auto load(ll::plugin::NativePlugin& self) -> bool {
    auto& logger       = self.getLogger();
    selfPluginInstance = std::make_unique<std::reference_wrapper<ll::plugin::NativePlugin>>(self);
    Raw_IniOpen(_CONFIG_FILE, "");
    ll::i18n::MultiFileI18N i18n;
    i18n.load(std::string("plugins/BackupHelper/LangPack/") + ini.GetValue("Main", "Language", "en_US") + ".json");
    ll::i18n::getInstance() = std::make_unique<ll::i18n::MultiFileI18N>(i18n);
    return true;
}

auto unload(ll::plugin::NativePlugin& self) -> bool { return true; }

} // namespace

auto getSelfPluginInstance() -> ll::plugin::NativePlugin& {
    if (!selfPluginInstance) {
        throw std::runtime_error("selfPluginInstance is null");
    }

    return *selfPluginInstance;
}

} // namespace BackupHelper

extern "C" {
_declspec(dllexport) auto ll_plugin_disable(ll::plugin::NativePlugin& self) -> bool {
    return BackupHelper::disable(self);
}
_declspec(dllexport) auto ll_plugin_enable(ll::plugin::NativePlugin& self) -> bool {
    return BackupHelper::enable(self);
}
_declspec(dllexport) auto ll_plugin_load(ll::plugin::NativePlugin& self) -> bool { return BackupHelper::load(self); }
_declspec(dllexport) auto ll_plugin_unload(ll::plugin::NativePlugin& self) -> bool {
    return BackupHelper::unload(self);
}
}
