#pragma once
#include "Entry.h"
#include "ll/api/service/Bedrock.h"
#include "mc/platform/UUID.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"

#include <string>

template <typename... Args>
inline void SendFeedback(mce::UUID uuid, const std::string& msg) {
    auto    level  = ll::service::getLevel();
    Player* player = nullptr;
    if (level.has_value() && (uuid != mce::UUID::EMPTY())) {
        player = level->getPlayer(uuid);
    }
    if (!player) {
        playerUuid = uuid;
    }
    if (!player) {
        backup_helper::getLogger().info(msg);
    } else try {
            player->sendMessage("§e[BackupHelper]§r " + msg);
        } catch (...) {
            playerUuid = mce::UUID::EMPTY();
            backup_helper::getLogger().info(msg);
        }
}
