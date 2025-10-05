#include "Interval.h"
#include "Backup.h"
#include "Entry.h"
#include "ll/api/chrono/GameChrono.h"
#include "ll/api/coro/CoroTask.h"
#include "ll/api/thread/ServerThreadExecutor.h"
#include <atomic>
#include <chrono>

std::atomic_bool isRunning = false;

std::chrono::seconds GetNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch());
}

void StartInterval() {
    long intervalHours = backup_helper::getConfig().GetLongValue("Main", "BackupInterval", 0);
    if (intervalHours == 0) {
        // Interval not set
        return;
    };

    isRunning = true;

    ll::coro::keepThis([intervalHours]() -> ll::coro::CoroTask<> {
        while (isRunning) {
            auto lastBackupTime    = backup_helper::getConfig().GetLongValue("BackFile", "lastTime", GetNow().count());
            auto lastBackupSeconds = std::chrono::seconds(lastBackupTime);

            // Seconds
            ll::chrono::game::ticks waitTime = (lastBackupSeconds + std::chrono::hours(intervalHours)) - GetNow();
            co_await (waitTime);

            if (!isRunning) co_return;

            if (!GetIsWorking()) {
                StartBackup();
                backup_helper::getConfig().SetLongValue("BackFile", "lastTime", GetNow().count());
                backup_helper::getConfig().SaveFile(backup_helper::getConfigPath().c_str());
            }

            co_await ll::chrono::game::ticks(20);
        }
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}

void StopInterval() { isRunning = false; };
