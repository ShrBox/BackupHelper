#pragma once
#include "Entry.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/utils/ErrorUtils.h"
#include "mc/platform/UUID.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#include <exception>
#include <string>


template <typename... Args>
inline void SendFeedback(mce::UUID uuid, const std::string& msg) {
    auto    level  = ll::service::getLevel();
    Player* player = nullptr;
    if (level.has_value() && uuid != mce::UUID::EMPTY()) {
        player = level->getPlayer(uuid);
    }
    if (!player) {
        extern mce::UUID playerUuid;
        playerUuid = uuid;
    }
    if (!player) {
        backup_helper::getLogger().info(msg);
    } else try {
            player->sendMessage("§e[BackupHelper]§r " + msg);
        } catch (...) {
            extern mce::UUID playerUuid;
            playerUuid = mce::UUID::EMPTY();
            backup_helper::getLogger().info(msg);
        }
}
