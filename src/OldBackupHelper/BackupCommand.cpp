#include "BackupCommand.h"
#include "Backup.h"
#include "ConfigFile.h"
#include "Tools.h"
#include "ll/api/chrono/GameChrono.h"
#include "ll/api/schedule/Task.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include <filesystem>
#include <ll/api/command/Command.h>
#include <ll/api/command/CommandHandle.h>
#include <ll/api/command/CommandRegistrar.h>
#include <ll/api/schedule/Scheduler.h>
#include <mc/world/actor/player/Player.h>

extern ll::schedule::GameTickScheduler scheduler;

void CmdReloadConfig(Player* p) {
    ini.Reset();
    auto res = ini.LoadFile(_CONFIG_FILE);
    if (res < 0) {
        SendFeedback(p, "Failed to open Config File!");
    } else {
        SendFeedback(p, "Config File reloaded.");
    }
}

void CmdBackup(Player* p) {
    Player* oldp = nowPlayer;
    nowPlayer    = p;
    if (isWorking) {
        SendFeedback(p, "An existing backup is working now...");
        nowPlayer = oldp;
    } else scheduler.add<ll::schedule::DelayTask>(ll::chrono::ticks(1), StartBackup);
}

void CmdCancel(Player* p) {
    if (isWorking) {
        isWorking = false;
        nowPlayer = nullptr;
        SendFeedback(p, "Backup is Canceled.");
    } else {
        SendFeedback(p, "No backup is working now.");
    }
    if (ini.GetBoolValue("BackFile", "isBack", false)) {
        ini.SetBoolValue("BackFile", "isBack", false);
        ini.SaveFile(_CONFIG_FILE);
        std::filesystem::remove_all("./plugins/BackupHelper/temp1/");
        SendFeedback(p, "Recover is Canceled.");
    }
}

// 接到回档指令，回档前文件解压
void CmdRecoverBefore(Player* p, int recover_Num) {
    Player* oldp = nowPlayer;
    nowPlayer    = p;
    if (isWorking) {
        SendFeedback(p, "An existing task is working now...Please wait and try again");
        nowPlayer = oldp;
    } else scheduler.add<ll::schedule::DelayTask>(ll::chrono::ticks(1), [recover_Num] { StartRecover(recover_Num); });
}
// 列出存在的存档备份
void CmdListBackup(Player* player, int limit) {
    backupList = getAllBackup();
    if (backupList.empty()) {
        SendFeedback(player, "No Backup Files");
        return;
    }
    int totalSize = backupList.size();
    int maxNum    = totalSize < limit ? totalSize : limit;
    SendFeedback(player, "使用存档文件前的数字选择回档文件");
    for (int i = 0; i < maxNum; i++) {
        SendFeedback(player, fmt::format("[{}]:{}", i, backupList[i].c_str()));
    }
}

// 重启时调用
void RecoverWorld() {
    bool isBack = ini.GetBoolValue("BackFile", "isBack", false);
    if (isBack) {
        SendFeedback(nullptr, "正在回档......");
        std::string worldName = ini.GetValue("BackFile", "worldName", "Bedrock level");
        if (!CopyRecoverFile(worldName)) {
            ini.SetBoolValue("BackFile", "isBack", false);
            ini.SaveFile(_CONFIG_FILE);
            SendFeedback(nullptr, "回档失败！");
            return;
        }
        ini.SetBoolValue("BackFile", "isBack", false);
        ini.SaveFile(_CONFIG_FILE);
        SendFeedback(nullptr, "回档成功");
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
                        .getOrCreateCommand("backup", "Create a backup", CommandPermissionLevel::GameDirectors);
    command.overload<BackupMainCommand>()
        .optional("backupOperation")
        .execute<
            [&](CommandOrigin const& origin, CommandOutput& output, BackupMainCommand const& param, Command const&) {
                if (!param.backupOperation) {
                    CmdBackup(static_cast<Player*>(origin.getEntity()));
                    return;
                }
                switch (param.backupOperation) {
                case BackupOperation::reload:
                    CmdReloadConfig(static_cast<Player*>(origin.getEntity()));
                    break;
                case BackupOperation::cancel:
                    CmdCancel(static_cast<Player*>(origin.getEntity()));
                    break;
                case BackupOperation::list:
                    CmdListBackup(static_cast<Player*>(origin.getEntity()), 100);
                    break;
                default:
                    output.error("Unknown operation");
                    break;
                }
            }>();
    command.overload<BackupRecoverCommand>()
        .text("recover")
        .required("recoverNumber")
        .execute<
            [&](CommandOrigin const& origin, CommandOutput& output, BackupRecoverCommand const& param, Command const&) {
                CmdRecoverBefore(static_cast<Player*>(origin.getEntity()), param.recoverNumber);
            }>();
}
