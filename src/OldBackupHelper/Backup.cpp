#include "Backup.h"
#include "ConfigFile.h"
#include "Tools.h"
#include "ll/api/chrono/GameChrono.h"
#include "ll/api/schedule/Task.h"
#include "ll/api/utils/ErrorUtils.h"
#include "mc/deps/core/mce/UUID.h"
#include <filesystem>
#include <ll/api/i18n/I18n.h>
#include <ll/api/memory/Hook.h>
#include <ll/api/schedule/Scheduler.h>
#include <mc/deps/core/string/HashedString.h>
#include <mc/locale/I18n.h>
#include <mc/locale/Localization.h>
#include <mc/server/commands/CommandContext.h>
#include <mc/server/commands/CommandOutput.h>
#include <mc/server/commands/MinecraftCommands.h>
#include <mc/server/commands/ServerCommandOrigin.h>
#include <mc/world/Minecraft.h>
#include <mc/world/actor/player/Player.h>
#include <mc/world/level/Command.h>
#include <mc/world/level/storage/DBStorage.h>
#include <regex>
#include <shellapi.h>
#include <string>
#include <thread>


#pragma comment(lib, "Shell32.lib")

#define TEMP_DIR  "./plugins/BackupHelper/temp/"
#define TEMP1_DIR "./plugins/BackupHelper/temp1/"
#define ZIP_PATH  "7za.exe"
using ll::i18n_literals::operator""_tr;

bool                     isWorking = false;
mce::UUID                playerUuid;
std::vector<std::string> backupList = {};

struct SnapshotFilenameAndLength {
    std::string path;
    size_t      size;
};

ll::schedule::GameTickScheduler scheduler;

void ResumeBackup();

void SuccessEnd() {
    SendFeedback(playerUuid, "Backup ended successfully"_tr());
    playerUuid = mce::UUID::EMPTY;
    // The isWorking assignment here has been moved to line 321
}

void FailEnd(int code = -1) {
    SendFeedback(playerUuid, "Failed to backup!"_tr() + (code == -1 ? "" : " Error code: {0}"_tr(code)));
    ResumeBackup();
    playerUuid = mce::UUID::EMPTY;
    isWorking  = false;
}

void ControlResourceUsage(HANDLE process) {
    // Job
    HANDLE hJob = CreateJobObject(NULL, L"BACKUP_HELPER_HELP_PROGRAM");
    if (hJob) {
        JOBOBJECT_BASIC_LIMIT_INFORMATION limit = {0};
        limit.PriorityClass                     = BELOW_NORMAL_PRIORITY_CLASS;
        limit.LimitFlags                        = JOB_OBJECT_LIMIT_PRIORITY_CLASS;

        SetInformationJobObject(hJob, JobObjectBasicLimitInformation, &limit, sizeof(limit));
        AssignProcessToJobObject(hJob, process);
    }

    // CPU Limit
    SYSTEM_INFO si;
    memset(&si, 0, sizeof(SYSTEM_INFO));
    GetSystemInfo(&si);
    DWORD cpuCnt  = si.dwNumberOfProcessors;
    DWORD cpuMask = 1;
    if (cpuCnt > 1) {
        if (cpuCnt % 2 == 1) cpuCnt -= 1;
        cpuMask = int(sqrt(1 << cpuCnt)) - 1; // sqrt(2^n)-1
    }
    SetProcessAffinityMask(process, cpuMask);
}

void ClearOldBackup() {
    int days = ini.GetLongValue("Main", "MaxStorageTime", -1);
    if (days < 0) return;
    SendFeedback(playerUuid, "Maximum backup retention time: {0} days"_tr(days));

    time_t       timeStamp = time(NULL) - days * 86400;
    std::wstring dirBackup = ll::string_utils::str2wstr(ini.GetValue("Main", "BackupPath", "backup"));
    std::wstring dirFind   = dirBackup + L"\\*";

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
            if (createTime.QuadPart / 10000000 - 11644473600 < (ULONGLONG)timeStamp) {
                DeleteFile((dirBackup + L"\\" + findFileData.cFileName).c_str());
                ++clearCount;
            }
        }
    } while (FindNextFile(hFind, &findFileData));
    FindClose(hFind);

    if (clearCount > 0) SendFeedback(playerUuid, "{0} old backups cleaned."_tr(clearCount));
    return;
}

void CleanTempDir() {
    std::error_code code;
    std::filesystem::remove_all(std::filesystem::path(TEMP_DIR), code);
}

bool CopyFiles(const std::string& worldName, std::vector<SnapshotFilenameAndLength>& files) {
    SendFeedback(playerUuid, "The list of files to be backed up has been captured. Processing..."_tr());
    SendFeedback(playerUuid, "Copying files..."_tr());

    // Copy Files
    CleanTempDir();
    std::error_code ec;
    std::filesystem::create_directories(TEMP_DIR, ec);
    ec.clear();

    std::filesystem::copy(
        ll::string_utils::str2wstr("./worlds/" + worldName),
        ll::string_utils::str2wstr(TEMP_DIR + worldName),
        std::filesystem::copy_options::recursive,
        ec
    );
    if (ec.value() != 0) {
        SendFeedback(playerUuid, "Failed to copy save files! {0}"_tr(ec.message()));
        FailEnd(GetLastError());
        return false;
    }

    // Truncate
    for (auto& file : files) {
        std::string toFile = TEMP_DIR + file.path;

        LARGE_INTEGER pos;
        pos.QuadPart = file.size;
        LARGE_INTEGER curPos;
        HANDLE        hSaveFile = CreateFileW(
            ll::string_utils::str2wstr(toFile).c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            0,
            0
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
        time_t nowtime = time(0);
        tm     info;
        localtime_s(&info, &nowtime);
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d_%H-%M-%S", &info);

        std::string backupPath = ini.GetValue("Main", "BackupPath", "backup");
        int         level      = ini.GetLongValue("Main", "Compress", 0);

        // Prepare command line
        char tmpParas[_MAX_PATH * 4] = {0};
        sprintf(
            tmpParas,
            "a \"%s\\%s_%s.7z\" \"%s%s\" -sdel -mx%d -mmt",
            backupPath.c_str(),
            worldName.c_str(),
            timeStr,
            TEMP_DIR,
            worldName.c_str(),
            level
        );

        wchar_t paras[_MAX_PATH * 4] = {0};
        ll::string_utils::str2wstr(tmpParas).copy(paras, strlen(tmpParas), 0);

        DWORD maxWait = ini.GetLongValue("Main", "MaxWaitForZip", 0);
        if (maxWait <= 0) maxWait = 0xFFFFFFFF;
        else maxWait *= 1000;

        // Start Process
        std::wstring     zipPath = ll::string_utils::str2wstr(ZIP_PATH);
        SHELLEXECUTEINFO sh      = {sizeof(SHELLEXECUTEINFO)};
        sh.fMask                 = SEE_MASK_NOCLOSEPROCESS;
        sh.hwnd                  = NULL;
        sh.lpVerb                = L"open";
        sh.nShow                 = SW_HIDE;
        sh.lpFile                = zipPath.c_str();
        sh.lpParameters          = paras;
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
    } catch (const ll::error_utils::seh_exception& e) {
        SendFeedback(playerUuid, "Exception in zip process! Error Code: {0}"_tr(e.code().value()) + ' ' + e.what());
        FailEnd(GetLastError());
        return false;
    } catch (const std::exception& e) {
        SendFeedback(playerUuid, "Exception in zip process! {0}"_tr(e.what()));
        FailEnd(GetLastError());
        return false;
    }
    return true;
}

bool UnzipFiles(const std::string& fileName) {
    try {
        // Get Name

        std::string backupPath = ini.GetValue("Main", "BackupPath", "backup");
        int         level      = ini.GetLongValue("Main", "Compress", 0);

        // Prepare command line
        char tmpParas[_MAX_PATH * 4] = {0};
        sprintf(tmpParas, "x \"%s\\%s\" -o%s", backupPath.c_str(), fileName.c_str(), TEMP1_DIR);

        wchar_t paras[_MAX_PATH * 4] = {0};
        ll::string_utils::str2wstr(tmpParas).copy(paras, strlen(tmpParas), 0);
        std::filesystem::remove_all(TEMP1_DIR);

        DWORD maxWait = ini.GetLongValue("Main", "MaxWaitForZip", 0);
        if (maxWait <= 0) maxWait = 0xFFFFFFFF;
        else maxWait *= 1000;

        // Start Process
        std::wstring     zipPath = ll::string_utils::str2wstr(ZIP_PATH);
        SHELLEXECUTEINFO sh      = {sizeof(SHELLEXECUTEINFO)};
        sh.fMask                 = SEE_MASK_NOCLOSEPROCESS;
        sh.hwnd                  = NULL;
        sh.lpVerb                = L"open";
        sh.nShow                 = SW_HIDE;
        sh.lpFile                = zipPath.c_str();
        sh.lpParameters          = paras;
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
    } catch (const ll::error_utils::seh_exception& e) {
        SendFeedback(playerUuid, "Exception in unzip process! Error Code: {0}"_tr(e.code().value()));
        // FailEnd(GetLastError());
        return false;
    } catch (const std::exception& e) {
        SendFeedback(playerUuid, "Exception in unzip process! {0}"_tr(e.what()));
        // FailEnd(GetLastError());
        return false;
    }
    ini.SetBoolValue("BackFile", "isBack", true);
    ini.SaveFile(_CONFIG_FILE);
    return true;
}

std::vector<std::string> getAllBackup() {
    std::string                      backupPath = ini.GetValue("Main", "BackupPath", "backup");
    std::filesystem::directory_entry entry(backupPath);
    std::regex                       isBackFile(".*7z");
    std::vector<std::string>         backupList;
    if (entry.status().type() == std::filesystem::file_type::directory) {
        for (const auto& iter : std::filesystem::directory_iterator(backupPath)) {
            std::string str = iter.path().filename().string();
            if (std::regex_match(str, isBackFile)) {
                backupList.push_back(str);
            }
        }
    }
    std::reverse(backupList.begin(), backupList.end());
    return backupList;
}

bool CopyRecoverFile(const std::string& worldName) {
    std::error_code error;
    // 判断回档文件存在
    auto file_status = std::filesystem::status(TEMP1_DIR + worldName, error);
    if (error) return false;
    if (!std::filesystem::exists(file_status)) return false;

    // 开始回档
    // 先重名原来存档，再复制回档文件
    auto file_status1 = std::filesystem::status("./worlds/" + worldName, error);
    if (error) return false;
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
}

bool StartBackup() {
    SendFeedback(playerUuid, "Backup process has been started"_tr());
    isWorking = true;
    ClearOldBackup();
    CommandContext context = CommandContext(
        "save hold",
        std::make_unique<ServerCommandOrigin>(
            ServerCommandOrigin("Server", ll::service::getLevel()->asServer(), CommandPermissionLevel::Internal, 0)
        )
    );
    try {
        ll::service::getMinecraft()->getCommands().executeCommand(context, false);
    } catch (const ll::error_utils::seh_exception& e) {
        SendFeedback(playerUuid, "Failed to start backup snapshot!");
        FailEnd(e.code().value());
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
        )
    );
    ll::service::getMinecraft()->getCommands().executeCommand(context, false);
    SendFeedback(
        playerUuid,
        "The rollback preparation has been completed. You can rollback by restarting. You can cancel the rollback by using the /backup cancel command."_tr(
        )
    ); // 回档准备已完成，重启可回档，使用 /backup cancel指令可取消回档
    backupList.clear();
    return true;
}

I18n& getI18n(); // Please remove it after 0.12.1 released

#define RETRY_TICKS 60

void ResumeBackup() {
    try {
        auto origin =
            ServerCommandOrigin("Server", ll::service::getLevel()->asServer(), CommandPermissionLevel::Internal, 0);
        auto command = ll::service::getMinecraft()->getCommands().compileCommand(
            HashedString("save resume"),
            origin,
            (CurrentCmdVersion)CommandVersion::CurrentVersion,
            [](std::string const& err) {}
        );
        CommandOutput output(CommandOutputType::AllOutput);
        std::string   outputStr;
        if (command) {
            command->run(origin, output);
            for (auto msg : output.getMessages()) {
                std::string temp;
                getI18n().getCurrentLanguage()->get(msg.getMessageId(), temp, msg.getParams());
                outputStr += temp.append("\n");
            }
            if (output.getMessages().size()) {
                outputStr.pop_back();
            }
        }
        if (!output.getSuccessCount()) {
            SendFeedback(playerUuid, "Failed to resume backup snapshot!"_tr());
            scheduler.add<ll::schedule::DelayTask>(ll::chrono::ticks(RETRY_TICKS), ResumeBackup);
        } else {
            SendFeedback(playerUuid, outputStr);
        }
    } catch (const ll::error_utils::seh_exception& e) {
        SendFeedback(playerUuid, "Failed to resume backup snapshot! Error Code: {0}"_tr(e.code().value()));
        if (isWorking) scheduler.add<ll::schedule::DelayTask>(ll::chrono::ticks(RETRY_TICKS), ResumeBackup);
    }
}

bool GetIsWorking() { return isWorking; }

LL_AUTO_TYPE_INSTANCE_HOOK(
    CreateSnapShotHook,
    ll::memory::HookPriority::Normal,
    DBStorage,
    "?createSnapshot@DBStorage@@UEAA?AV?$vector@USnapshotFilenameAndLength@@V?$allocator@USnapshotFilenameAndLength@@@"
    "std@@@std@@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@3@_N@Z",
    std::vector<struct SnapshotFilenameAndLength>,
    std::string const& worldName,
    bool               idk
) {
    if (isWorking) {
        ini.SetValue("BackFile", "worldName", worldName.c_str());
        ini.SaveFile(_CONFIG_FILE);
        auto files = origin(worldName, idk);
        if (CopyFiles(worldName, files)) {
            std::thread([worldName]() {
                ll::error_utils::setSehTranslator();
                ZipFiles(worldName);
                CleanTempDir();
                SuccessEnd();
            }).detach();
        }

        scheduler.add<ll::schedule::DelayTask>(ll::chrono::ticks(20), ResumeBackup);
        return files;
    } else {
        isWorking = true; // Prevent the backup command from being accidentally executed during a map hang
        return origin(worldName, idk);
    }
}

LL_AUTO_TYPE_INSTANCE_HOOK(
    ReleaseSnapShotHook,
    HookPriority::Normal,
    DBStorage,
    "?releaseSnapshot@DBStorage@@UEAAXXZ",
    void
) {
    isWorking = false;
    origin();
}
