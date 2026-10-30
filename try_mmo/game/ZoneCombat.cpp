#include "Zone.h"

#include "Balance.h"

#include <algorithm>
#include <cmath>

namespace trymmo
{
using namespace balance;

namespace
{
// 사거리에 정확히 멈춘 뒤의 부동소수점 오차를 흡수한다.
constexpr float kRangeTolerance = 0.01f;

// 사거리 밖이면 사거리 경계까지만 다가간다. 다가간 뒤의 거리를 돌려준다.
float approach(Vec2& position, Vec2 target, float step)
{
    const float distance = distanceBetween(position, target);
    if (distance <= kAttackRange)
    {
        return distance;
    }
    moveToward(position, target, std::min(step, distance - kAttackRange));
    return distanceBetween(position, target);
}
}

void Zone::updatePlayers()
{
    for (const auto& [id, player] : players_)
    {
        if (player->reviveTick)
        {
            if (tick_ >= *player->reviveTick)
            {
                revivePlayer(*player);
            }
            continue;
        }

        updatePlayerRegen(*player);
        if (player->target)
        {
            updatePlayerAttack(*player);
        }
        else if (player->destination && moveToward(player->position, *player->destination, kPlayerStepPerTick))
        {
            player->destination.reset();
        }
    }
}

void Zone::updatePlayerRegen(Player& player)
{
    if (tick_ >= player.nextRegenTick)
    {
        player.hp = std::min(player.hp + 1, player.maxHp);
        player.nextRegenTick = tick_ + kPlayerRegenPeriodTicks;
    }
}

void Zone::updatePlayerAttack(Player& player)
{
    Slime& slime = *slimes_.at(*player.target);
    const float distance = approach(player.position, slime.position, kPlayerStepPerTick);
    const bool inRange = distance <= kAttackRange + kRangeTolerance;
    player.approaching = !inRange;
    if (!inRange || tick_ < player.nextAttackTick)
    {
        return;
    }

    player.nextAttackTick = tick_ + kPlayerAttackPeriodTicks;
    const int damage = rollDamage(player.attack, kBasicAttackCoefficient, kSlimeDefense);
    slime.hp = std::max(slime.hp - damage, 0);
    broadcastCombat({player.id, slime.id, damage, slime.hp});

    if (slime.hp == 0)
    {
        killSlime(slime);
        return;
    }

    // 전투 중에는 회복하지 않고, 가장 최근 공격자를 쫓는다.
    slime.mode = SlimeMode::Chase;
    slime.chaseTarget = player.id;
    slime.destination.reset();
}

void Zone::revivePlayer(Player& player)
{
    player.reviveTick.reset();
    player.position = kEnterPosition;
    player.hp = player.maxHp;
    player.nextRegenTick = tick_ + kPlayerRegenPeriodTicks;
}

void Zone::killPlayer(Player& player)
{
    const std::uint32_t reviveTick = tick_ + kPlayerReviveTicks;
    player.reviveTick = reviveTick;
    player.target.reset();
    player.destination.reset();
    player.approaching = false;

    for (const auto& [viewerId, viewer] : players_)
    {
        if (viewerId == player.id || viewer->visible.contains(player.id))
        {
            observer_.onPlayerDeath(viewerId, player.id, reviveTick);
        }
    }
}

void Zone::updateSlimes()
{
    for (const auto& [id, slime] : slimes_)
    {
        switch (slime->mode)
        {
        case SlimeMode::Wander:
            updateWander(*slime);
            break;
        case SlimeMode::Chase:
            updateChase(*slime);
            break;
        case SlimeMode::Return:
            updateReturn(*slime);
            break;
        case SlimeMode::Dead:
            if (tick_ >= slime->respawnTick)
            {
                respawnSlime(*slime);
            }
            break;
        }
    }
}

void Zone::updateWander(Slime& slime)
{
    if (slime.destination)
    {
        if (moveToward(slime.position, *slime.destination, kSlimeWanderStepPerTick))
        {
            slime.destination.reset();
        }
    }
    else if (tick_ >= slime.nextWanderTick)
    {
        slime.destination = randomPointNear(slime.spawnPoint, kSlimeWanderRadius);
        slime.nextWanderTick = tick_ + randomWanderDelay();
    }
}

void Zone::updateChase(Slime& slime)
{
    const auto found = players_.find(slime.chaseTarget);
    if (found == players_.end() || found->second->reviveTick
        || distanceBetween(slime.position, slime.spawnPoint) > kSlimeLeashDistance)
    {
        startReturn(slime);
        return;
    }

    Player& player = *found->second;
    const float distance = approach(slime.position, player.position, kSlimeChaseStepPerTick);
    const bool inRange = distance <= kAttackRange + kRangeTolerance;
    slime.approaching = !inRange;
    if (!inRange || tick_ < slime.nextAttackTick)
    {
        return;
    }

    slime.nextAttackTick = tick_ + kSlimeAttackPeriodTicks;
    const int damage = rollDamage(kSlimeAttack, kBasicAttackCoefficient, player.defense);
    player.hp = std::max(player.hp - damage, 0);
    broadcastCombat({slime.id, player.id, damage, player.hp});

    if (player.hp == 0)
    {
        killPlayer(player);
    }
}

void Zone::updateReturn(Slime& slime)
{
    if (moveToward(slime.position, slime.spawnPoint, kSlimeReturnStepPerTick))
    {
        slime.mode = SlimeMode::Wander;
        slime.hp = kSlimeMaxHp;
        slime.nextWanderTick = tick_ + randomWanderDelay();
    }
}

void Zone::respawnSlime(Slime& slime)
{
    // 등장 알림은 이번 틱의 AOI 갱신이 보낸다.
    slime.mode = SlimeMode::Wander;
    slime.position = slime.spawnPoint;
    slime.hp = kSlimeMaxHp;
    slime.nextWanderTick = tick_ + randomWanderDelay();
}

void Zone::startReturn(Slime& slime)
{
    slime.mode = SlimeMode::Return;
    slime.approaching = false;
    clearTargetsOn(slime.id);
}

void Zone::killSlime(Slime& slime)
{
    slime.mode = SlimeMode::Dead;
    slime.respawnTick = tick_ + kSlimeRespawnTicks;
    slime.destination.reset();
    slime.approaching = false;
    clearTargetsOn(slime.id);

    for (const auto& [viewerId, viewer] : players_)
    {
        if (viewer->visible.erase(slime.id) > 0)
        {
            observer_.onEntityDespawn(viewerId, slime.id, DespawnReason::Died);
        }
    }
}

int Zone::rollDamage(int attack, float coefficient, int defense)
{
    std::uniform_real_distribution<float> random(kDamageRandomMin, kDamageRandomMax);
    const float damage = attack * coefficient * 100.0f / (100.0f + defense) * random(random_);
    return std::max(static_cast<int>(std::lround(damage)), 1);
}

void Zone::clearTargetsOn(EntityId slime)
{
    for (const auto& [id, player] : players_)
    {
        if (player->target == slime)
        {
            player->target.reset();
            player->approaching = false;
        }
    }
}

void Zone::broadcastCombat(const CombatEvent& event)
{
    for (const auto& [viewerId, viewer] : players_)
    {
        if (viewerId == event.attacker || viewerId == event.target
            || viewer->visible.contains(event.attacker) || viewer->visible.contains(event.target))
        {
            observer_.onCombat(viewerId, event);
        }
    }
}

std::uint32_t Zone::randomWanderDelay()
{
    return static_cast<std::uint32_t>(
        std::uniform_int_distribution<int>(kSlimeWanderMinTicks, kSlimeWanderMaxTicks)(random_));
}
}
