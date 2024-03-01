#pragma once
#include "Entry.h"
#include "mc/deps/core/mce/UUID.h"
#include <exception>
#include <ll/api/Logger.h>
#include <ll/api/service/Bedrock.h>
#include <ll/api/utils/ErrorUtils.h>
#include <mc/world/actor/player/Player.h>
#include <mc/world/level/Level.h>
#include <string>


template <typename... Args>
inline void SendFeedback(mce::UUID uuid, const std::string& msg) {
    bool    found = false;
    auto    level = ll::service::getLevel();
    Player* player;
    if (level.has_value() && !uuid.isEmpty()) {
        if ((player = level->getPlayer(uuid))) {
            found = true;
        }
    }
    if (!found) {
        extern mce::UUID playerUuid;
        playerUuid = uuid;
    }
    if (!found || uuid.isEmpty()) BackupHelper::getSelfPluginInstance().getLogger().info(msg);
    else {
        try {
            // p->sendTextPacket("§e[BackupHelper]§r " + msg, TextType::RAW);
            player->sendMessage("§e[BackupHelper]§r " + msg);
        } catch (const ll::error_utils::seh_exception&) {
            extern mce::UUID playerUuid;
            playerUuid = mce::UUID::EMPTY;
            BackupHelper::getSelfPluginInstance().getLogger().info(msg);
        } catch (const std::exception&) {
            extern mce::UUID playerUuid;
            playerUuid = mce::UUID::EMPTY;
            BackupHelper::getSelfPluginInstance().getLogger().info(msg);
        }
    }
}
