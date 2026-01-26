#include "Backup.h"
#include "Entry.h"
#include "Tools.h"
#include "ll/api/chrono/GameChrono.h"
#include "ll/api/coro/CoroTask.h"
#include "ll/api/i18n/I18n.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/thread/ServerThreadExecutor.h"
#include "ll/api/utils/ErrorUtils.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/core/utility/MCRESULT.h"
#include "mc/locale/I18n.h"
#include "mc/locale/Localization.h"
#include "mc/locale/OptionalString.h"
#include "mc/server/commands/Command.h"
#include "mc/server/commands/CommandContext.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandVersion.h"
#include "mc/server/commands/MinecraftCommands.h"
#include "mc/server/commands/ServerCommandOrigin.h"
#include "mc/world/Minecraft.h"
#include "mc/world/level/storage/DBStorage.h"

#include <filesystem>
#include <regex>
#include <shellapi.h>
#include <string>
#include <thread>


#pragma comment(lib, "Shell32.lib")

#define TEMP_DIR  backup_helper::BackupHelper::getInstance().getSelf().getModDir() / "temp"
#define TEMP1_DIR backup_helper::BackupHelper::getInstance().getSelf().getModDir() / "temp1"
#define ZIP_PATH  "7za.exe"
using ll::i18n_literals::operator""_tr;

bool                     isWorking = false;
mce::UUID                playerUuid;
std::vector<std::string> backupList = {};

struct SnapshotFilenameAndLength {
    std::string path;
    size_t      size;
};

void ResumeBackup();

void SuccessEnd() {
    SendFeedback(playerUuid, "Backup ended successfully"_tr());
    playerUuid = mce::UUID::EMPTY();
    // The isWorking assignment here has been moved to line 321
}

void FailEnd(int code = -1) {
    SendFeedback(playerUuid, "Failed to backup!"_tr() + (code == -1 ? "" : " Error code: {0}"_tr(code)));
    ResumeBackup();
    playerUuid = mce::UUID::EMPTY();
    isWorking  = false;
}

void ControlResourceUsage(HANDLE process) {
    // Job
    if (HANDLE hJob = CreateJobObject(nullptr, L"BACKUP_HELPER_HELP_PROGRAM")) {
        JOBOBJECT_BASIC_LIMIT_INFORMATION limit = {0};
        limit.PriorityClass                     = BELOW_NORMAL_PRIORITY_CLASS;
        limit.LimitFlags                        = JOB_OBJECT_LIMIT_PRIORITY_CLASS;

        SetInformationJobObject(hJob, JobObjectBasicLimitInformation, &limit, sizeof(limit));
        AssignProcessToJobObject(hJob, process);
    }

    // CPU Limit
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    DWORD cpuCnt  = si.dwNumberOfProcessors;
    DWORD cpuMask = 1;
    if (cpuCnt > 1) {
        if (cpuCnt % 2 == 1) cpuCnt -= 1;
        cpuMask = static_cast<int>(sqrt(1 << cpuCnt)) - 1; // sqrt(2^n)-1
    }
    SetProcessAffinityMask(process, cpuMask);
}

void ClearOldBackup() {
    int days = backup_helper::getConfig().GetLongValue("Main", "MaxStorageTime", -1);
    if (days < 0) return;
    SendFeedback(playerUuid, "Maximum backup retention time: {0} days"_tr(days));

    time_t       timeStamp = time(nullptr) - days * 86400;
    std::wstring dirBackup =
        ll::string_utils::str2wstr(backup_helper::getConfig().GetValue("Main", "BackupPath", "backup"));
    std::wstring dirFind = dirBackup + L"\\*";

    WIN32_FIND_DATA findFileData;
    ULARGE_INTEGER  createTime;
    int             clearCount = 0;

    HANDLE hFind = FindFirstFile(dirFind.c_str(), &findFileData);
    if (hFind == INVALID_HANDLE_VALUE) {
        SendFeedback(playerUuid, "Fail to locate old backups."_tr());
        return;
    }
    do {
        if (findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        else {
            createTime.LowPart  = findFileData.ftCreationTime.dwLowDateTime;
            createTime.HighPart = findFileData.ftCreationTime.dwHighDateTime;
            if (createTime.QuadPart / 10000000 - 11644473600 < static_cast<ULONGLONG>(timeStamp)) {
                DeleteFile((dirBackup + L"\\" + findFileData.cFileName).c_str());
                ++clearCount;
            }
        }
    } while (FindNextFile(hFind, &findFileData));
    FindClose(hFind);

    if (clearCount > 0) SendFeedback(playerUuid, "{0} old backups cleaned."_tr(clearCount));
}

void CleanTempDir() {
    std::error_code code;
    std::filesystem::remove_all(std::filesystem::path(TEMP_DIR), code);
}

bool CopyFiles(const std::string& worldName, std::vector<SnapshotFilenameAndLength> const& files) {
    SendFeedback(playerUuid, "The list of files to be backed up has been captured. Processing..."_tr());
    SendFeedback(playerUuid, "Copying files..."_tr());

    // Copy Files
    CleanTempDir();
    std::error_code ec;
    std::filesystem::create_directories(TEMP_DIR, ec);
    ec.clear();

    std::filesystem::copy("./worlds/" + worldName, TEMP_DIR / worldName, std::filesystem::copy_options::recursive, ec);
    if (ec.value() != 0) {
        SendFeedback(playerUuid, "Failed to copy save files!"_tr() + " " + ec.message());
        FailEnd(GetLastError());
        return false;
    }

    // Truncate
    for (auto& file : files) {
        std::string toFile = (TEMP_DIR / file.path).string();

        LARGE_INTEGER pos;
        pos.QuadPart = file.size;
        LARGE_INTEGER curPos;
        HANDLE        hSaveFile = CreateFileW(
            ll::string_utils::str2wstr(toFile).c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );

        if (hSaveFile == INVALID_HANDLE_VALUE || !SetFilePointerEx(hSaveFile, pos, &curPos, FILE_BEGIN)
            || !SetEndOfFile(hSaveFile)) {
            SendFeedback(playerUuid, "Failed to truncate {0}!"_tr(toFile));
            FailEnd(GetLastError());
            return false;
        }
        CloseHandle(hSaveFile);
    }
    SendFeedback(playerUuid, "The compression process may take quite some time, please be patient."_tr());
    return true;
}

bool ZipFiles(const std::string& worldName) {
    try {
        // Get Name
        char   timeStr[32];
        time_t nowtime = time(nullptr);
        tm     info;
        localtime_s(&info, &nowtime);
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d_%H-%M-%S", &info);

        std::string backupPath = backup_helper::getConfig().GetValue("Main", "BackupPath", "backup");
        int         level      = backup_helper::getConfig().GetLongValue("Main", "Compress", 0);

        using namespace ll::string_utils;
        // Prepare command line
        auto paras = str2wstr(
            fmt::format(
                R"(a "{}\{}_{}.{}" "{}/{}" -sdel -mx{} -mmt)",
                backupPath,
                worldName,
                timeStr,
                backup_helper::getConfig().GetValue("Main", "ArchiveFormat", "7z"),
                u8str2str((TEMP_DIR).u8string()),
                worldName,
                level
            )
        );

        DWORD maxWait = backup_helper::getConfig().GetLongValue("Main", "MaxWaitForZip", 0);
        if (maxWait <= 0) maxWait = 0xFFFFFFFF;
        else maxWait *= 1000;

        // Start Process
        std::wstring     zipPath = str2wstr(ZIP_PATH);
        SHELLEXECUTEINFO sh      = {sizeof(SHELLEXECUTEINFO)};
        sh.fMask                 = SEE_MASK_NOCLOSEPROCESS;
        sh.hwnd                  = nullptr;
        sh.lpVerb                = L"open";
        sh.nShow                 = SW_HIDE;
        sh.lpFile                = zipPath.c_str();
        sh.lpParameters          = paras.c_str();
        if (!ShellExecuteEx(&sh)) {
            SendFeedback(playerUuid, "Fail to create Zip process!"_tr());
            FailEnd(GetLastError());
            return false;
        }

        ControlResourceUsage(sh.hProcess);
        SetPriorityClass(sh.hProcess, BELOW_NORMAL_PRIORITY_CLASS);

        // Wait
        DWORD res;
        if ((res = WaitForSingleObject(sh.hProcess, maxWait)) == WAIT_TIMEOUT || res == WAIT_FAILED) {
            SendFeedback(playerUuid, "Zip process timeout!"_tr());
            FailEnd(GetLastError());
        }
        CloseHandle(sh.hProcess);
    } catch (const std::exception& e) {
        SendFeedback(playerUuid, "Exception in zip process! "_tr() + " " + e.what());
        FailEnd(GetLastError());
        return false;
    } catch (...) {
        SendFeedback(playerUuid, "Exception in unzip process!");
        ll::error_utils::printCurrentException(backup_helper::getLogger());
        // FailEnd(GetLastError());
        return false;
    }
    return true;
}

bool UnzipFiles(const std::string& fileName) {
    try {
        // Get Name
        std::string backupPath = backup_helper::getConfig().GetValue("Main", "BackupPath", "backup");

        using namespace ll::string_utils;

        auto paras =
            str2wstr(fmt::format(R"(x "{}\{}" -o"{}")", backupPath, fileName, u8str2str((TEMP1_DIR).u8string())));
        std::filesystem::remove_all(TEMP_DIR);

        DWORD maxWait = backup_helper::getConfig().GetLongValue("Main", "MaxWaitForZip", 0);
        if (maxWait <= 0) maxWait = 0xFFFFFFFF;
        else maxWait *= 1000;

        // Start Process
        std::wstring     zipPath = str2wstr(ZIP_PATH);
        SHELLEXECUTEINFO sh      = {sizeof(SHELLEXECUTEINFO)};
        sh.fMask                 = SEE_MASK_NOCLOSEPROCESS;
        sh.hwnd                  = nullptr;
        sh.lpVerb                = L"open";
        sh.nShow                 = SW_HIDE;
        sh.lpFile                = zipPath.c_str();
        sh.lpParameters          = paras.c_str();
        if (!ShellExecuteEx(&sh)) {
            SendFeedback(playerUuid, "Fail to Unzip process!"_tr());
            // FailEnd(GetLastError());
            return false;
        }

        ControlResourceUsage(sh.hProcess);
        SetPriorityClass(sh.hProcess, BELOW_NORMAL_PRIORITY_CLASS);

        // Wait
        DWORD res;
        if ((res = WaitForSingleObject(sh.hProcess, maxWait)) == WAIT_TIMEOUT || res == WAIT_FAILED) {
            SendFeedback(playerUuid, "Unzip process timeout!"_tr());
            // FailEnd(GetLastError());
        }
        CloseHandle(sh.hProcess);
    } catch (const std::exception& e) {
        SendFeedback(playerUuid, "Exception in unzip process! "_tr() + " " + e.what());
        // FailEnd(GetLastError());
        return false;
    } catch (...) {
        SendFeedback(playerUuid, "Exception in unzip process!");
        ll::error_utils::printCurrentException(backup_helper::getLogger());
        // FailEnd(GetLastError());
        return false;
    }

    backup_helper::getConfig().SetBoolValue("BackFile", "isBack", true);
    backup_helper::getConfig().SaveFile(backup_helper::getConfigPath().c_str());
    return true;
}

std::vector<std::string> getAllBackup() {
    std::string                      backupPath = backup_helper::getConfig().GetValue("Main", "BackupPath", "backup");
    std::filesystem::directory_entry entry(backupPath);
    std::regex isBackFile(fmt::format(".*{}", backup_helper::getConfig().GetValue("Main", "ArchiveFormat", "7z")));
    std::vector<std::string> result;
    if (entry.status().type() == std::filesystem::file_type::directory) {
        for (const auto& iter : std::filesystem::directory_iterator(backupPath)) {
            std::string str = iter.path().filename().string();
            if (std::regex_match(str, isBackFile)) {
                result.push_back(str);
            }
        }
    }
    std::ranges::reverse(result);
    return result;
}

bool CopyRecoverFile(const std::string& worldName) {
    std::error_code error;
    // 判断回档文件存在
    auto file_status = std::filesystem::status(TEMP1_DIR / worldName, error);
    if (error) return false;
    if (!std::filesystem::exists(file_status)) return false;

    // 开始回档
    // 先重名原来存档，再复制回档文件
    auto file_status1 = std::filesystem::status("./worlds/" + worldName, error);
    if (error) return false;
    try {
        if (std::filesystem::exists(file_status1) && std::filesystem::exists(file_status)) {
            std::filesystem::rename("./worlds/" + worldName, "./worlds/" + worldName + "_bak");
        } else {
            return false;
        }
        std::filesystem::copy(TEMP1_DIR, "./worlds", std::filesystem::copy_options::recursive, error);
        if (error.value() != 0) {
            SendFeedback(playerUuid, "Failed to copy files!\n" + error.message());
            std::filesystem::remove_all(TEMP1_DIR);
            std::filesystem::rename("./worlds/" + worldName + "_bak", "./worlds/" + worldName);
            return false;
        }
        std::filesystem::remove_all(TEMP1_DIR);
        std::filesystem::remove_all("./worlds/" + worldName + "_bak");
        return true;
    } catch (...) {
        ll::error_utils::printCurrentException(backup_helper::getLogger());
        return false;
    }
}

bool StartBackup() {
    SendFeedback(playerUuid, "Backup process has been started"_tr());
    isWorking = true;
    ClearOldBackup();
    CommandContext context = CommandContext(
        "save hold",
        std::make_unique<ServerCommandOrigin>(
            ServerCommandOrigin("Server", ll::service::getLevel()->asServer(), CommandPermissionLevel::Internal, 0)
        ),
        CommandVersion::CurrentVersion()
    );
    try {
        ll::service::getMinecraft()->mCommands->executeCommand(context, false);
    } catch (...) {
        SendFeedback(playerUuid, "Failed to start backup snapshot!");
        ll::error_utils::printCurrentException(backup_helper::getLogger());
        FailEnd();
        return false;
    }
    return true;
}

bool StartRecover(int recover_NUM) {
    SendFeedback(playerUuid, "Preparation before rollback has been started"_tr());
    if (backupList.empty()) {
        SendFeedback(
            playerUuid,
            "The internal archive list of the plugin is empty, please use the list subcommand and try again."_tr()
        );
        return false;
    }
    unsigned int i = recover_NUM + 1;
    if (i > backupList.size()) {
        SendFeedback(
            playerUuid,
            "The archive selection parameters are not within the number of existing archives, please select again."_tr()
        );
        return false;
    }
    isWorking = true;
    SendFeedback(playerUuid, "The archive file is being decompressed and copied."_tr());
    if (!UnzipFiles(backupList[recover_NUM])) {
        SendFeedback(playerUuid, "Rollback file preparation failed"_tr());
        isWorking = false;
        return false;
    };
    SendFeedback(
        playerUuid,
        "The rollback file preparation is completed and the backup before rollback is about to be carried out"_tr()
    );
    CommandContext context = CommandContext(
        "save hold",
        std::make_unique<ServerCommandOrigin>(
            ServerCommandOrigin("Server", ll::service::getLevel()->asServer(), CommandPermissionLevel::Internal, 0)
        ),
        CommandVersion::CurrentVersion()
    );
    ll::service::getMinecraft()->mCommands->executeCommand(context, false);
    SendFeedback(
        playerUuid,
        "The rollback preparation has been completed. You can rollback by restarting. You can cancel the rollback by using the /backup cancel command."_tr()
    ); // 回档准备已完成，重启可回档，使用 /backup cancel指令可取消回档
    backupList.clear();
    return true;
}

constexpr unsigned short RETRY_TICKS = 60;
constexpr unsigned short MAX_RETRY   = 2;

void ResumeBackup() {
    try {
        auto origin =
            ServerCommandOrigin("Server", ll::service::getLevel()->asServer(), CommandPermissionLevel::Internal, 0);
        auto command = ll::service::getMinecraft()->mCommands->compileCommand(
            HashedString("save resume"),
            origin,
            static_cast<CurrentCmdVersion>(CommandVersion::CurrentVersion()),
            [](std::string const&) {}
        );
        CommandOutput output(CommandOutputType::AllOutput);
        std::string   outputStr;
        if (command) {
            command->run(origin, output);
            for (const auto& msg : output.mMessages) {
                auto opStr = getI18n().getCurrentLanguage()->_get(msg.mMessageId, msg.mParams);
                if (opStr.valid) {
                    outputStr += opStr.string.get() + "\n";
                }
            }
            if (!output.mMessages.empty()) {
                outputStr.pop_back();
            }
        }
        if (!output.mSuccessCount) {
            SendFeedback(playerUuid, "Failed to resume backup snapshot!"_tr());
            SendFeedback(playerUuid, outputStr);
            static unsigned short retry = 0;
            ll::coro::keepThis([]() -> ll::coro::CoroTask<> {
                co_await ll::chrono::ticks(RETRY_TICKS);
                if (retry++ < MAX_RETRY) {
                    ResumeBackup();
                } else {
                    retry = 0;
                }
            }).launch(ll::thread::ServerThreadExecutor::getDefault());
        } else {
            SendFeedback(playerUuid, outputStr);
        }
    } catch (...) {
        SendFeedback(playerUuid, "Failed to resume backup snapshot!");
        if (isWorking) {
            ll::coro::keepThis([]() -> ll::coro::CoroTask<> {
                co_await ll::chrono::ticks(RETRY_TICKS);
                ResumeBackup();
            }).launch(ll::thread::ServerThreadExecutor::getDefault());
        }
    }
}

bool GetIsWorking() { return isWorking; }

LL_AUTO_TYPE_INSTANCE_HOOK(
    CreateSnapShotHook,
    ll::memory::HookPriority::Normal,
    DBStorage,
    &DBStorage::$createSnapshot,
    std::vector<struct SnapshotFilenameAndLength>,
    std::string const& worldName,
    bool               flushWriteCache
) {
    if (isWorking) {
        backup_helper::getConfig().SetValue("BackFile", "worldName", worldName.c_str());
        backup_helper::getConfig().SaveFile(backup_helper::getConfigPath().c_str());
        auto files = origin(worldName, flushWriteCache);
        if (CopyFiles(worldName, files)) {
            std::thread([worldName]() {
                ZipFiles(worldName);
                CleanTempDir();
                SuccessEnd();
            }).detach();
        }

        ll::coro::keepThis([]() -> ll::coro::CoroTask<> {
            co_await ll::chrono::ticks(20);
            ResumeBackup();
        }).launch(ll::thread::ServerThreadExecutor::getDefault());
        return files;
    } else {
        isWorking = true; // Prevent the backup command from being accidentally executed during a map hang
        return origin(worldName, flushWriteCache);
    }
}

LL_AUTO_TYPE_INSTANCE_HOOK(ReleaseSnapShotHook, HookPriority::Normal, DBStorage, &DBStorage::$releaseSnapshot, void) {
    isWorking = false;
    origin();
}
