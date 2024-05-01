#include "BackupCommand.h"
#include "Backup.h"
#include "ConfigFile.h"
#include "Tools.h"
#include "ll/api/chrono/GameChrono.h"
#include "ll/api/i18n/I18n.h"
#include "ll/api/schedule/Task.h"
#include "mc/deps/core/mce/UUID.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include <filesystem>
#include <ll/api/command/Command.h>
#include <ll/api/command/CommandHandle.h>
#include <ll/api/command/CommandRegistrar.h>
#include <ll/api/schedule/Scheduler.h>
#include <mc/world/actor/player/Player.h>

extern ll::schedule::GameTickScheduler scheduler;
using ll::i18n_literals::operator""_tr;

void CmdReloadConfig(mce::UUID uuid) {
    ini.Reset();
    auto res = ini.LoadFile(_CONFIG_FILE);
    if (res < 0) {
        SendFeedback(uuid, "Failed to open Config File!"_tr());
    } else {
        SendFeedback(uuid, "Config File reloaded."_tr());
    }
}

void CmdBackup(mce::UUID uuid) {
    mce::UUID oldUuid = playerUuid;
    playerUuid        = uuid;
    if (isWorking) {
        SendFeedback(uuid, "An existing backup is working now..."_tr());
        playerUuid = oldUuid;
    } else scheduler.add<ll::schedule::DelayTask>(ll::chrono::ticks(1), StartBackup);
}

void CmdCancel(mce::UUID uuid) {
    if (isWorking) {
        isWorking  = false;
        playerUuid = mce::UUID::EMPTY;
        SendFeedback(uuid, "Backup is Canceled."_tr());
    } else {
        SendFeedback(uuid, "No backup is working now."_tr());
    }
    if (ini.GetBoolValue("BackFile", "isBack", false)) {
        ini.SetBoolValue("BackFile", "isBack", false);
        ini.SaveFile(_CONFIG_FILE);
        std::filesystem::remove_all("./plugins/BackupHelper/temp1/");
        SendFeedback(uuid, "Recover is Canceled."_tr());
    }
}

// 接到回档指令，回档前文件解压
void CmdRecoverBefore(mce::UUID uuid, int recover_Num) {
    mce::UUID oldUuid = playerUuid;
    playerUuid        = uuid;
    if (isWorking) {
        SendFeedback(uuid, "An existing task is working now...Please wait and try again"_tr());
        playerUuid = oldUuid;
    } else scheduler.add<ll::schedule::DelayTask>(ll::chrono::ticks(1), [recover_Num] { StartRecover(recover_Num); });
}
// 列出存在的存档备份
void CmdListBackup(mce::UUID uuid, int limit) {
    backupList = getAllBackup();
    if (backupList.empty()) {
        SendFeedback(uuid, "No Backup Files"_tr());
        return;
    }
    int totalSize = backupList.size();
    int maxNum    = totalSize < limit ? totalSize : limit;
    SendFeedback(uuid, "Select the rollback file using the number before the archive file"_tr());
    for (int i = 0; i < maxNum; i++) {
        SendFeedback(uuid, fmt::format("[{}]:{}", i, backupList[i].c_str()));
    }
}

// 重启时调用
void RecoverWorld() {
    bool isBack = ini.GetBoolValue("BackFile", "isBack", false);
    if (isBack) {
        SendFeedback(mce::UUID::EMPTY, "Rollbacking..."_tr());
        std::string worldName = ini.GetValue("BackFile", "worldName", "Bedrock level");
        if (!CopyRecoverFile(worldName)) {
            ini.SetBoolValue("BackFile", "isBack", false);
            ini.SaveFile(_CONFIG_FILE);
            SendFeedback(mce::UUID::EMPTY, "Failed to rollback"_tr());
            return;
        }
        ini.SetBoolValue("BackFile", "isBack", false);
        ini.SaveFile(_CONFIG_FILE);
        SendFeedback(mce::UUID::EMPTY, "Rollback successfully"_tr());
    }
}

enum BackupOperation : int { reload = 1, cancel = 2, list = 3 };
struct BackupMainCommand {
    BackupOperation backupOperation;
};
struct BackupRecoverCommand {
    int recoverNumber;
};

void RegisterCommand() {
    using ll::command::CommandRegistrar;
    auto& command = ll::command::CommandRegistrar::getInstance()
                        .getOrCreateCommand("backup", "Create a backup"_tr(), CommandPermissionLevel::GameDirectors);
    command.overload<BackupMainCommand>()
        .optional("backupOperation")
        .execute([&](CommandOrigin const&     origin,
                     CommandOutput&           output,
                     BackupMainCommand const& param,
                     Command const&) {
            switch (param.backupOperation) {
            case BackupOperation::reload:
                CmdReloadConfig(
                    origin.getEntity() ? static_cast<Player*>(origin.getEntity())->getUuid() : mce::UUID::EMPTY
                );
                break;
            case BackupOperation::cancel:
                CmdCancel(origin.getEntity() ? static_cast<Player*>(origin.getEntity())->getUuid() : mce::UUID::EMPTY);
                break;
            case BackupOperation::list:
                CmdListBackup(
                    origin.getEntity() ? static_cast<Player*>(origin.getEntity())->getUuid() : mce::UUID::EMPTY,
                    100
                );
                break;
            default:
                CmdBackup(origin.getEntity() ? static_cast<Player*>(origin.getEntity())->getUuid() : mce::UUID::EMPTY);
                break;
            }
        });
    command.overload<BackupRecoverCommand>()
        .text("recover")
        .required("recoverNumber")
        .execute(
            [&](CommandOrigin const& origin, CommandOutput& output, BackupRecoverCommand const& param, Command const&) {
                CmdRecoverBefore(
                    origin.getEntity() ? static_cast<Player*>(origin.getEntity())->getUuid() : mce::UUID::EMPTY,
                    param.recoverNumber
                );
            }
        );
}
