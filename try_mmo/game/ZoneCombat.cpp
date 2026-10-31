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

// 최고 레벨이면 0.
int requiredExpFor(int level)
{
    if (level >= kPlayerMaxLevel)
    {
        return 0;
    }
    return static_cast<int>(std::lround(kRequiredExpBase * std::pow(kRequiredExpGrowth, level - 1)));
}

bool hasMoved(Vec2 before, Vec2 after)
{
    return before.x != after.x || before.y != after.y;
}

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
        player->moved = false;
        player->rolled = false;
        if (player->reviveTick)
        {
            if (tick_ >= *player->reviveTick)
            {
                revivePlayer(*player);
            }
            continue;
        }

        updatePlayerRegen(*player);
        if (player->rollTicksLeft > 0)
        {
            // 구르는 동안 기본 공격을 보류한다. 대상과 공격 주기는 그대로 둔다.
            updateRoll(*player);
        }
        else if (player->target)
        {
            updatePlayerAttack(*player);
        }
        else if (player->destination)
        {
            const Vec2 before = player->position;
            if (moveToward(player->position, *player->destination, kPlayerStepPerTick))
            {
                player->destination.reset();
            }
            player->moved = hasMoved(before, player->position);
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
    const Vec2 before = player.position;
    const float distance = approach(player.position, slime.position, kPlayerStepPerTick);
    player.moved = hasMoved(before, player.position);
    if (distance > kAttackRange + kRangeTolerance || tick_ < player.nextAttackTick)
    {
        return;
    }

    player.nextAttackTick = tick_ + kPlayerAttackPeriodTicks;
    hitSlime(player, slime, kBasicAttackCoefficient, SkillKind::None);
}

bool Zone::hitSlime(Player& player, Slime& slime, float coefficient, SkillKind skill)
{
    const int damage = rollDamage(player.attack, coefficient, kSlimeDefense);
    slime.contributions[player.id] += std::min(damage, slime.hp);
    slime.hp = std::max(slime.hp - damage, 0);
    broadcastCombat({player.id, slime.id, damage, slime.hp, skill});

    if (slime.hp == 0)
    {
        killSlime(slime, player.id);
        return false;
    }

    // 전투 중에는 회복하지 않고, 가장 최근 공격자를 쫓는다.
    slime.mode = SlimeMode::Chase;
    slime.chaseTarget = player.id;
    slime.destination.reset();
    return true;
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
    player.pendingDestination.reset();
    player.pendingTarget.reset();
    player.rollTicksLeft = 0;  // 이동만 멈춘다. 쿨다운은 유지한다
    player.rollHits.clear();

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
        const Vec2 before = slime->position;
        slime->stunned = tick_ < slime->stunEndTick;
        if (slime->stunned)
        {
            // 기절 중에는 모드를 유지한 채 아무것도 하지 않는다. 풀리면 이전 행동을 잇는다.
            slime->moved = false;
            continue;
        }

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
        slime->moved = slime->mode != SlimeMode::Dead && hasMoved(before, slime->position);
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
    if (distance > kAttackRange + kRangeTolerance || tick_ < slime.nextAttackTick)
    {
        return;
    }

    slime.nextAttackTick = tick_ + kSlimeAttackPeriodTicks;
    const float defense = player.rolled ? player.defense * kRollDefenseMultiplier : static_cast<float>(player.defense);
    const int damage = rollDamage(kSlimeAttack, kBasicAttackCoefficient, defense);
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
        slime.contributions.clear();
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
    clearTargetsOn(slime.id);
}

void Zone::killSlime(Slime& slime, EntityId killer)
{
    slime.mode = SlimeMode::Dead;
    slime.respawnTick = tick_ + kSlimeRespawnTicks;
    slime.destination.reset();
    clearTargetsOn(slime.id);

    for (const auto& [viewerId, viewer] : players_)
    {
        if (viewer->visible.erase(slime.id) > 0)
        {
            observer_.onEntityDespawn(viewerId, slime.id, DespawnReason::Died);
        }
    }

    grantRewards(slime, killer);
}

void Zone::grantRewards(Slime& slime, EntityId killer)
{
    std::uniform_int_distribution<int> gold(kSlimeGoldMin, kSlimeGoldMax);
    for (const auto& [playerId, damage] : slime.contributions)
    {
        // 이미 나간 플레이어는 받지 못한다. 사망 중인 플레이어는 받는다.
        const auto found = players_.find(playerId);
        if (found == players_.end() || (damage < kContributionDamage && playerId != killer))
        {
            continue;
        }

        Player& player = *found->second;
        RewardEvent reward;
        reward.slime = slime.id;
        reward.slimePosition = slime.position;
        reward.goldGained = gold(random_);
        player.gold += reward.goldGained;
        if (player.level < kPlayerMaxLevel)
        {
            reward.expGained = options_.slimeKillExp;
            reward.levelsGained = gainExp(player, reward.expGained);
        }
        reward.progress = progressOf(playerId);
        observer_.onReward(playerId, reward);
    }
    slime.contributions.clear();
}

int Zone::gainExp(Player& player, int exp)
{
    int levels = 0;
    player.exp += exp;
    while (player.level < kPlayerMaxLevel && player.exp >= requiredExpFor(player.level))
    {
        player.exp -= requiredExpFor(player.level);
        levelUp(player);
        ++levels;
    }
    if (player.level == kPlayerMaxLevel)
    {
        player.exp = 0;
    }
    return levels;
}

void Zone::levelUp(Player& player)
{
    ++player.level;
    player.maxHp += kLevelUpMaxHp;
    player.attack += kLevelUpAttack;
    player.defense += kLevelUpDefense;
    // 사망 중이면 HP는 0으로 두고 부활할 때 회복한다.
    if (!player.reviveTick)
    {
        player.hp = player.maxHp;
    }
}

PlayerProgress Zone::progressOf(EntityId id) const
{
    const Player& player = *players_.at(id);
    return {player.level, player.exp, requiredExpFor(player.level), player.gold,
            player.maxHp, player.attack, player.defense};
}

int Zone::rollDamage(int attack, float coefficient, float defense)
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
