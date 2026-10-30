#include "Zone.h"

#include <cmath>
#include <numbers>

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

constexpr int kSlimeCount = 30;
constexpr int kSlimeMaxHp = 120;
constexpr int kSlimeLevel = 1;
constexpr float kSlimeSpawnRadius = 600.0f;
constexpr float kSlimeSafeRadius = 100.0f;  // 입장 위치 주변은 생성 구역에서 뺀다
constexpr float kSlimeWanderRadius = 100.0f;
constexpr float kSlimeWanderStepPerTick = 4.0f;  // 초당 40
constexpr int kSlimeWanderMinTicks = 30;
constexpr int kSlimeWanderMaxTicks = 60;

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

// 도착하면 true. 남은 거리가 이동량 이하이면 목적지에 정확히 놓는다.
bool moveToward(Vec2& position, Vec2 destination, float step)
{
    const float remaining = std::sqrt(distanceSquared(position, destination));
    if (remaining <= step)
    {
        position = destination;
        return true;
    }

    const float ratio = step / remaining;
    position.x += (destination.x - position.x) * ratio;
    position.y += (destination.y - position.y) * ratio;
    return false;
}
}

Zone::Zone(IZoneObserver& observer)
    : observer_(observer), random_(std::random_device{}())
{
    spawnSlimes();
}

std::optional<EntityView> Zone::addPlayer()
{
    if (players_.size() >= kMaxPlayers)
    {
        return std::nullopt;
    }

    auto player = std::make_unique<Player>();
    player->id = nextEntityId_++;
    player->name = "Player" + std::to_string(nextPlayerNumber_++);
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
    updateSlimes();
    updateVisibility();
    sendPositions();
}

void Zone::spawnSlimes()
{
    for (int i = 0; i < kSlimeCount; ++i)
    {
        auto slime = std::make_unique<Slime>();
        slime->id = nextEntityId_++;
        slime->spawnPoint = randomSlimeSpawnPoint();
        slime->position = slime->spawnPoint;
        slime->hp = kSlimeMaxHp;
        slime->nextWanderTick = std::uniform_int_distribution<int>(0, kSlimeWanderMaxTicks)(random_);
        slimes_.emplace(slime->id, std::move(slime));
    }
}

Vec2 Zone::randomSlimeSpawnPoint()
{
    // 원 안 고른 분포에서 입장 위치 주변만 다시 뽑는다.
    for (;;)
    {
        const Vec2 point = randomPointNear(kEnterPosition, kSlimeSpawnRadius);
        if (distanceSquared(point, kEnterPosition) > kSlimeSafeRadius * kSlimeSafeRadius)
        {
            return point;
        }
    }
}

Vec2 Zone::randomPointNear(Vec2 center, float radius)
{
    // 반지름에 제곱근을 씌워야 원 안에 고르게 퍼진다.
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const float distance = radius * std::sqrt(unit(random_));
    const float angle = 2.0f * std::numbers::pi_v<float> * unit(random_);
    return {center.x + distance * std::cos(angle), center.y + distance * std::sin(angle)};
}

void Zone::movePlayers()
{
    for (const auto& [id, player] : players_)
    {
        if (player->destination && moveToward(player->position, *player->destination, kMoveStepPerTick))
        {
            player->destination.reset();
        }
    }
}

void Zone::updateSlimes()
{
    std::uniform_int_distribution<int> wanderDelay(kSlimeWanderMinTicks, kSlimeWanderMaxTicks);
    for (const auto& [id, slime] : slimes_)
    {
        if (slime->destination)
        {
            if (moveToward(slime->position, *slime->destination, kSlimeWanderStepPerTick))
            {
                slime->destination.reset();
            }
        }
        else if (tick_ >= slime->nextWanderTick)
        {
            slime->destination = randomPointNear(slime->spawnPoint, kSlimeWanderRadius);
            slime->nextWanderTick = tick_ + wanderDelay(random_);
        }
    }
}

void Zone::updateVisibility()
{
    // P1~P4는 100명 이하이므로 전체 쌍을 단순 거리로 계산한다. 공간 분할은 P5에서 다룬다.
    for (const auto& [viewerId, viewer] : players_)
    {
        for (const auto& [otherId, other] : players_)
        {
            if (viewerId != otherId)
            {
                updateVisibilityOf(*viewer, otherId, other->position, makeView(*other));
            }
        }
        for (const auto& [slimeId, slime] : slimes_)
        {
            updateVisibilityOf(*viewer, slimeId, slime->position, makeView(*slime));
        }
    }
}

void Zone::updateVisibilityOf(Player& viewer, EntityId other, Vec2 otherPosition, const EntityView& view)
{
    constexpr float enter = kAoiEnterDistance * kAoiEnterDistance;
    constexpr float leave = kAoiLeaveDistance * kAoiLeaveDistance;

    const float distance = distanceSquared(viewer.position, otherPosition);
    const bool isVisible = viewer.visible.contains(other);
    if (!isVisible && distance <= enter)
    {
        viewer.visible.insert(other);
        observer_.onEntitySpawn(viewer.id, view);
    }
    else if (isVisible && distance > leave)
    {
        viewer.visible.erase(other);
        observer_.onEntityDespawn(viewer.id, other, DespawnReason::OutOfAoi);
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
            entities.push_back(makeSnapshot(otherId));
        }
        observer_.onPositionSync(viewerId, tick_, entities);
    }
}

EntitySnapshot Zone::makeSnapshot(EntityId id) const
{
    if (const auto player = players_.find(id); player != players_.end())
    {
        return makeSnapshot(*player->second);
    }
    return makeSnapshot(*slimes_.at(id));
}

EntityView Zone::makeView(const Player& player)
{
    return {player.id, EntityType::Player, player.name, kPlayerMaxHp, player.position,
            player.destination ? EntityState::Moving : EntityState::Idle};
}

EntityView Zone::makeView(const Slime& slime)
{
    return {slime.id, EntityType::Slime, "푸른 슬라임", kSlimeMaxHp, slime.position,
            slime.destination ? EntityState::Moving : EntityState::Idle};
}

EntitySnapshot Zone::makeSnapshot(const Player& player)
{
    return {player.id, player.position, player.destination ? EntityState::Moving : EntityState::Idle,
            player.hp, player.level};
}

EntitySnapshot Zone::makeSnapshot(const Slime& slime)
{
    return {slime.id, slime.position, slime.destination ? EntityState::Moving : EntityState::Idle,
            slime.hp, kSlimeLevel};
}
}
