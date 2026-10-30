#include "Zone.h"

#include <cmath>

namespace trymmo
{
namespace
{
constexpr float kMapSize = 1600.0f;
constexpr Vec2 kEnterPosition{800.0f, 800.0f};
constexpr std::size_t kMaxPlayers = 100;
constexpr int kPlayerMaxHp = 100;
constexpr int kPlayerStartLevel = 1;
constexpr float kMoveStepPerTick = 10.0f;  // 초당 100, 10Hz
constexpr float kAoiEnterDistance = 700.0f;
constexpr float kAoiLeaveDistance = 800.0f;

float distanceSquared(Vec2 a, Vec2 b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

// NaN도 비교가 거짓이 되어 여기서 걸러진다.
bool isInsideMap(Vec2 position)
{
    return position.x >= 0.0f && position.x <= kMapSize
        && position.y >= 0.0f && position.y <= kMapSize;
}
}

std::optional<EntityView> Zone::addPlayer()
{
    if (players_.size() >= kMaxPlayers)
    {
        return std::nullopt;
    }

    auto player = std::make_unique<Player>();
    player->id = nextEntityId_++;
    player->name = "Player" + std::to_string(player->id);
    player->position = kEnterPosition;
    player->hp = kPlayerMaxHp;
    player->level = kPlayerStartLevel;

    EntityView view = makeView(*player);
    players_.emplace(player->id, std::move(player));
    return view;
}

void Zone::removePlayer(EntityId id)
{
    players_.erase(id);
    for (const auto& [viewerId, viewer] : players_)
    {
        if (viewer->visible.erase(id) > 0)
        {
            observer_.onEntityDespawn(viewerId, id, DespawnReason::Left);
        }
    }
}

bool Zone::setDestination(EntityId id, Vec2 destination)
{
    if (!isInsideMap(destination))
    {
        return false;
    }

    players_.at(id)->destination = destination;
    return true;
}

void Zone::tick()
{
    ++tick_;
    movePlayers();
    updateVisibility();
    sendPositions();
}

void Zone::movePlayers()
{
    for (const auto& [id, player] : players_)
    {
        if (!player->destination)
        {
            continue;
        }

        const Vec2 destination = *player->destination;
        const float remaining = std::sqrt(distanceSquared(player->position, destination));
        if (remaining <= kMoveStepPerTick)
        {
            player->position = destination;
            player->destination.reset();
            continue;
        }

        const float ratio = kMoveStepPerTick / remaining;
        player->position.x += (destination.x - player->position.x) * ratio;
        player->position.y += (destination.y - player->position.y) * ratio;
    }
}

void Zone::updateVisibility()
{
    constexpr float enter = kAoiEnterDistance * kAoiEnterDistance;
    constexpr float leave = kAoiLeaveDistance * kAoiLeaveDistance;

    // P1은 100명 이하이므로 전체 쌍을 단순 거리로 계산한다. 공간 분할은 P5에서 다룬다.
    for (const auto& [viewerId, viewer] : players_)
    {
        for (const auto& [otherId, other] : players_)
        {
            if (viewerId == otherId)
            {
                continue;
            }

            const float distance = distanceSquared(viewer->position, other->position);
            const bool isVisible = viewer->visible.contains(otherId);
            if (!isVisible && distance <= enter)
            {
                viewer->visible.insert(otherId);
                observer_.onEntitySpawn(viewerId, makeView(*other));
            }
            else if (isVisible && distance > leave)
            {
                viewer->visible.erase(otherId);
                observer_.onEntityDespawn(viewerId, otherId, DespawnReason::OutOfAoi);
            }
        }
    }
}

void Zone::sendPositions()
{
    std::vector<EntitySnapshot> entities;
    for (const auto& [viewerId, viewer] : players_)
    {
        entities.clear();
        entities.push_back(makeSnapshot(*viewer));
        for (const EntityId otherId : viewer->visible)
        {
            entities.push_back(makeSnapshot(*players_.at(otherId)));
        }
        observer_.onPositionSync(viewerId, tick_, entities);
    }
}

EntityView Zone::makeView(const Player& player)
{
    return {player.id, player.name, kPlayerMaxHp, player.position,
            player.destination ? EntityState::Moving : EntityState::Idle};
}

EntitySnapshot Zone::makeSnapshot(const Player& player)
{
    return {player.id, player.position, player.destination ? EntityState::Moving : EntityState::Idle,
            player.hp, player.level};
}
}
