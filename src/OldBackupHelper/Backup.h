#pragma once
#include <string>
#include <vector>

class Player;

extern Player*                  nowPlayer;
extern std::vector<std::string> backupList;
extern bool                     isWorking;

bool                     StartBackup();
bool                     StartRecover(int recover_NUM);
bool                     CopyRecoverFile(const std::string& worldName);
bool                     GetIsWorking();
std::vector<std::string> getAllBackup();
