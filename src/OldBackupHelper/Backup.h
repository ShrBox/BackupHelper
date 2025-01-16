#pragma once
#include "mc/platform/UUID.h"
#include <string>
#include <vector>

extern mce::UUID                playerUuid;
extern std::vector<std::string> backupList;
extern bool                     isWorking;

bool                     StartBackup();
bool                     StartRecover(int recover_NUM);
bool                     CopyRecoverFile(const std::string& worldName);
bool                     GetIsWorking();
std::vector<std::string> getAllBackup();
