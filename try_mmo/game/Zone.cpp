#include "Zone.h"

#include "Balance.h"

#include <cmath>
#include <numbers>

namespace trymmo
{
using namespace balance;

namespace
{
// NaN도 비교가 거짓이 되어 여기서 걸러진다.
bool isInsideMap(Vec2 position)
{
    return position.x >= 0.0f && position.x <= kMapSize
        && position.y >= 0.0f && position.y <= kMapSize;
}
}

float distanceBetween(Vec2 a, Vec2 b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

bool moveToward(Vec2& position, Vec2 destination, float step)
{
    const float remaining = distanceBetween(position, destination);
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

Zone::Zone(IZoneObserver& observer, ZoneOptions options)
    : observer_(observer), options_(options), random_(std::random_device{}())
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
    player->hp = options_.playerMaxHp;
    player->maxHp = options_.playerMaxHp;
    player->level = kPlayerStartLevel;
    player->attack = options_.playerAttack;
    player->defense = kPlayerDefense;
    player->nextRegenTick = tick_ + kPlayerRegenPeriodTicks;

    EntityView view = makeView(*player);
    players_.emplace(player->id, std::move(player));
    return view;
}

void Zone::removePlayer(EntityId id)
{
    // 이 플레이어를 쫓던 슬라임은 다음 틱에 대상이 없음을 보고 복귀한다.
    players_.erase(id);
    for (const auto& [viewerId, viewer] : players_)
    {
        if (viewer->visible.erase(id) > 0)
        {
            observer_.onEntityDespawn(viewerId, id, DespawnReason::Left);
        }
    }
}

MoveResult Zone::setDestination(EntityId id, Vec2 destination)
{
    if (!isInsideMap(destination))
    {
        return MoveResult::OutOfMap;
    }

    Player& player = *players_.at(id);
    if (player.reviveTick)
    {
        return MoveResult::PlayerDead;
    }

    player.destination = destination;
    player.target.reset();
    return MoveResult::Accepted;
}

TargetResult Zone::setTarget(EntityId id, EntityId target)
{
    Player& player = *players_.at(id);
    if (player.reviveTick)
    {
        return TargetResult::PlayerDead;
    }

    const auto found = slimes_.find(target);
    if (found == slimes_.end())
    {
        return TargetResult::NotFound;
    }
    if (found->second->mode == SlimeMode::Dead)
    {
        return TargetResult::TargetDead;
    }
    if (found->second->mode == SlimeMode::Return)
    {
        return TargetResult::TargetReturning;
    }

    // 공격 주기 타이머는 건드리지 않는다. 대상 재지정으로 공격이 빨라지면 안 된다.
    player.target = target;
    player.destination.reset();
    return TargetResult::Accepted;
}

void Zone::tick()
{
    ++tick_;
    updatePlayers();
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
        slime->nextWanderTick = randomWanderDelay();
        slimes_.emplace(slime->id, std::move(slime));
    }
}

Vec2 Zone::randomSlimeSpawnPoint()
{
    // 원 안 고른 분포에서 입장 위치 주변만 다시 뽑는다.
    for (;;)
    {
        const Vec2 point = randomPointNear(kEnterPosition, kSlimeSpawnRadius);
        if (distanceBetween(point, kEnterPosition) > kSlimeSafeRadius)
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
            if (slime->mode != SlimeMode::Dead)
            {
                updateVisibilityOf(*viewer, slimeId, slime->position, makeView(*slime));
            }
        }
    }
}

void Zone::updateVisibilityOf(Player& viewer, EntityId other, Vec2 otherPosition, const EntityView& view)
{
    const float distance = distanceBetween(viewer.position, otherPosition);
    const bool isVisible = viewer.visible.contains(other);
    if (!isVisible && distance <= kAoiEnterDistance)
    {
        viewer.visible.insert(other);
        observer_.onEntitySpawn(viewer.id, view);
    }
    else if (isVisible && distance > kAoiLeaveDistance)
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
    return {player.id, EntityType::Player, player.name, player.maxHp, player.position, stateOf(player)};
}

EntityView Zone::makeView(const Slime& slime)
{
    return {slime.id, EntityType::Slime, "푸른 슬라임", kSlimeMaxHp, slime.position, stateOf(slime)};
}

EntitySnapshot Zone::makeSnapshot(const Player& player)
{
    return {player.id, player.position, stateOf(player), player.hp, player.level, player.maxHp};
}

EntitySnapshot Zone::makeSnapshot(const Slime& slime)
{
    return {slime.id, slime.position, stateOf(slime), slime.hp, kSlimeLevel, kSlimeMaxHp};
}

EntityState Zone::stateOf(const Player& player)
{
    if (player.reviveTick)
    {
        return EntityState::Dead;
    }
    return player.moved ? EntityState::Moving : EntityState::Idle;
}

EntityState Zone::stateOf(const Slime& slime)
{
    if (slime.mode == SlimeMode::Return)
    {
        return EntityState::Returning;
    }
    return slime.moved ? EntityState::Moving : EntityState::Idle;
}
}
