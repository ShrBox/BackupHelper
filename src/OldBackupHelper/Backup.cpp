#include "Backup.h"
#include "ConfigFile.h"
#include "Tools.h"
#include "ll/api/chrono/GameChrono.h"
#include "ll/api/schedule/Task.h"
#include "ll/api/utils/ErrorUtils.h"
#include <filesystem>
#include <ll/api/memory/Hook.h>
#include <ll/api/schedule/Scheduler.h>
#include <mc/deps/core/string/HashedString.h>
#include <mc/locale/I18n.h>
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
#define ZIP_PATH  ".\\plugins\\BackupHelper\\7za.exe"

bool                     isWorking  = false;
Player*                  nowPlayer  = nullptr;
std::vector<std::string> backupList = {};

struct SnapshotFilenameAndLength {
    std::string path;
    size_t      size;
};

ll::schedule::GameTickScheduler scheduler;

void ResumeBackup();

void SuccessEnd() {
    SendFeedback(nowPlayer, "备份成功结束");
    nowPlayer = nullptr;
    // The isWorking assignment here has been moved to line 321
}

void FailEnd(int code = -1) {
    SendFeedback(nowPlayer, std::string("备份失败！") + (code == -1 ? "" : "错误码：" + std::to_string(code)));
    ResumeBackup();
    nowPlayer = nullptr;
    isWorking = false;
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
    SendFeedback(nowPlayer, "备份最长保存时间：" + std::to_string(days) + "天");

    time_t       timeStamp = time(NULL) - days * 86400;
    std::wstring dirBackup = ll::string_utils::str2wstr(ini.GetValue("Main", "BackupPath", "backup"));
    std::wstring dirFind   = dirBackup + L"\\*";

    WIN32_FIND_DATA findFileData;
    ULARGE_INTEGER  createTime;
    int             clearCount = 0;

    HANDLE hFind = FindFirstFile(dirFind.c_str(), &findFileData);
    if (hFind == INVALID_HANDLE_VALUE) {
        SendFeedback(nowPlayer, "Fail to locate old backups.");
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

    if (clearCount > 0) SendFeedback(nowPlayer, std::to_string(clearCount) + " old backups cleaned.");
    return;
}

void CleanTempDir() {
    std::error_code code;
    std::filesystem::remove_all(std::filesystem::path(TEMP_DIR), code);
}

bool CopyFiles(const std::string& worldName, std::vector<SnapshotFilenameAndLength>& files) {
    SendFeedback(nowPlayer, "已抓取到BDS待备份文件清单。正在处理...");
    SendFeedback(nowPlayer, "正在复制文件...");

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
        SendFeedback(nowPlayer, "Failed to copy save files!\n" + ec.message());
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
            SendFeedback(nowPlayer, "Failed to truncate " + toFile + "!");
            FailEnd(GetLastError());
            return false;
        }
        CloseHandle(hSaveFile);
    }
    SendFeedback(nowPlayer, "压缩过程可能花费相当长的时间，请耐心等待");
    return true;
}

bool ZipFiles(const std::string& worldName) {
    try {
        // Get Name
        char   timeStr[32];
        time_t nowtime;
        time(&nowtime);
        struct tm* info = localtime(&nowtime);
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d_%H-%M-%S", info);

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
            SendFeedback(nowPlayer, "Fail to create Zip process!");
            FailEnd(GetLastError());
            return false;
        }

        ControlResourceUsage(sh.hProcess);
        SetPriorityClass(sh.hProcess, BELOW_NORMAL_PRIORITY_CLASS);

        // Wait
        DWORD res;
        if ((res = WaitForSingleObject(sh.hProcess, maxWait)) == WAIT_TIMEOUT || res == WAIT_FAILED) {
            SendFeedback(nowPlayer, "Zip process timeout!");
            FailEnd(GetLastError());
        }
        CloseHandle(sh.hProcess);
    } catch (const ll::error_utils::seh_exception& e) {
        SendFeedback(nowPlayer, "Exception in zip process! Error Code:" + std::to_string(e.code().value()));
        FailEnd(GetLastError());
        return false;
    } catch (const std::exception& e) {
        SendFeedback(nowPlayer, std::string("Exception in zip process!\n") + e.what());
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
            SendFeedback(nowPlayer, "Fail to Unzip process!");
            // FailEnd(GetLastError());
            return false;
        }

        ControlResourceUsage(sh.hProcess);
        SetPriorityClass(sh.hProcess, BELOW_NORMAL_PRIORITY_CLASS);

        // Wait
        DWORD res;
        if ((res = WaitForSingleObject(sh.hProcess, maxWait)) == WAIT_TIMEOUT || res == WAIT_FAILED) {
            SendFeedback(nowPlayer, "Unzip process timeout!");
            // FailEnd(GetLastError());
        }
        CloseHandle(sh.hProcess);
    } catch (const ll::error_utils::seh_exception& e) {
        SendFeedback(nowPlayer, "Exception in unzip process! Error Code:" + std::to_string(e.code().value()));
        // FailEnd(GetLastError());
        return false;
    } catch (const std::exception& e) {
        SendFeedback(nowPlayer, std::string("Exception in unzip process!\n") + e.what());
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
        SendFeedback(nowPlayer, "Failed to copy files!\n" + error.message());
        std::filesystem::remove_all(TEMP1_DIR);
        std::filesystem::rename("./worlds/" + worldName + "_bak", "./worlds/" + worldName);
        return false;
    }
    std::filesystem::remove_all(TEMP1_DIR);
    std::filesystem::remove_all("./worlds/" + worldName + "_bak");
    return true;
}

bool StartBackup() {
    SendFeedback(nowPlayer, "备份已启动");
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
        SendFeedback(nowPlayer, "Failed to start backup snapshot!");
        FailEnd(e.code().value());
        return false;
    }
    return true;
}

bool StartRecover(int recover_NUM) {
    SendFeedback(nowPlayer, "回档前准备已启动");
    if (backupList.empty()) {
        SendFeedback(nowPlayer, "插件内部存档列表为空，请使用list子指令后再试");
        return false;
    }
    unsigned int i = recover_NUM + 1;
    if (i > backupList.size()) {
        SendFeedback(nowPlayer, "存档选择参数不在已有存档数内，请重新选择");
        return false;
    }
    isWorking = true;
    SendFeedback(nowPlayer, "正在进行回档文件解压复制");
    if (!UnzipFiles(backupList[recover_NUM])) {
        SendFeedback(nowPlayer, "回档文件准备失败");
        isWorking = false;
        return false;
    };
    SendFeedback(nowPlayer, "回档文件准备完成,即将进行回档前备份");
    CommandContext context = CommandContext(
        "save hold",
        std::make_unique<ServerCommandOrigin>(
            ServerCommandOrigin("Server", ll::service::getLevel()->asServer(), CommandPermissionLevel::Internal, 0)
        )
    );
    ll::service::getMinecraft()->getCommands().executeCommand(context, false);
    SendFeedback(nowPlayer, "回档准备已完成，重启可回档,使用backup cancel指令可取消回档");
    backupList.clear();
    return true;
}

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
                outputStr = outputStr.append(I18n::get(msg.getMessageId(), msg.getParams())).append("\n");
            }
            if (output.getMessages().size()) {
                outputStr.pop_back();
            }
        }
        if (!output.getSuccessCount()) {
            SendFeedback(nowPlayer, "Failed to resume backup snapshot!");
            scheduler.add<ll::schedule::DelayTask>(ll::chrono::ticks(RETRY_TICKS), ResumeBackup);
        } else {
            SendFeedback(nowPlayer, outputStr);
        }
    } catch (const ll::error_utils::seh_exception& e) {
        SendFeedback(nowPlayer, "Failed to resume backup snapshot! Error Code:" + std::to_string(e.code().value()));
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
