#pragma once
#include "Entry.h"
#include <exception>
#include <ll/api/Logger.h>
#include <ll/api/service/Bedrock.h>
#include <ll/api/utils/ErrorUtils.h>
#include <mc/world/actor/player/Player.h>
#include <mc/world/level/Level.h>
#include <string>


template <typename... Args>
inline void SendFeedback(Player* p, const std::string& msg) {
    bool found = false;
    ll::service::getLevel()->forEachPlayer([&p, &found](Player& player) {
        if (player == *p) {
            found = true;
        }
        return true;
    });
    if (!found) {
        extern Player* nowPlayer;
        nowPlayer = p = nullptr;
    }

    if (!p) BackupHelper::getSelfPluginInstance().getLogger().info(msg);
    else {
        try {
            // p->sendTextPacket("§e[BackupHelper]§r " + msg, TextType::RAW);
            p->sendMessage("§e[BackupHelper]§r " + msg);
        } catch (const ll::error_utils::seh_exception&) {
            extern Player* nowPlayer;
            nowPlayer = nullptr;
            BackupHelper::getSelfPluginInstance().getLogger().info(msg);
        } catch (const std::exception&) {
            extern Player* nowPlayer;
            nowPlayer = nullptr;
            BackupHelper::getSelfPluginInstance().getLogger().info(msg);
        }
    }
}
