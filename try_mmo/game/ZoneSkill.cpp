#include "Zone.h"

#include "Balance.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace trymmo
{
using namespace balance;

namespace
{
bool isUnitVector(Vec2 value)
{
    // NaN은 비교가 거짓이 되어 여기서 걸러진다.
    const float length = std::sqrt(value.x * value.x + value.y * value.y);
    return length >= 1.0f - kDirectionTolerance && length <= 1.0f + kDirectionTolerance;
}

// 직선을 따라 움직일 수 있는 거리. 맵 경계에서 멈추고 벽을 타고 미끄러지지 않는다.
float distanceToBoundary(Vec2 position, Vec2 direction, float wanted)
{
    float distance = wanted;
    if (direction.x > 0.0f)
    {
        distance = std::min(distance, (kMapSize - position.x) / direction.x);
    }
    else if (direction.x < 0.0f)
    {
        distance = std::min(distance, -position.x / direction.x);
    }
    if (direction.y > 0.0f)
    {
        distance = std::min(distance, (kMapSize - position.y) / direction.y);
    }
    else if (direction.y < 0.0f)
    {
        distance = std::min(distance, -position.y / direction.y);
    }
    return std::max(distance, 0.0f);
}

// 점에서 선분까지의 거리.
float distanceToSegment(Vec2 point, Vec2 start, Vec2 end)
{
    const float dx = end.x - start.x;
    const float dy = end.y - start.y;
    const float lengthSquared = dx * dx + dy * dy;
    if (lengthSquared == 0.0f)
    {
        return distanceBetween(point, start);
    }

    const float dot = (point.x - start.x) * dx + (point.y - start.y) * dy;
    const float ratio = std::clamp(dot / lengthSquared, 0.0f, 1.0f);
    return distanceBetween(point, {start.x + dx * ratio, start.y + dy * ratio});
}
}

SkillOutcome Zone::useSkill(EntityId id, SkillKind skill, Vec2 direction)
{
    Player& player = *players_.at(id);
    if (player.reviveTick)
    {
        return {SkillResult::PlayerDead, 0};
    }
    if (skill != SkillKind::Roll && skill != SkillKind::SpinKick)
    {
        return {SkillResult::UnknownSkill, 0};
    }
    if (player.rollTicksLeft > 0)
    {
        return {SkillResult::Rolling, 0};
    }

    std::uint32_t& cooldownEnd = skill == SkillKind::Roll ? player.rollCooldownEndTick : player.spinCooldownEndTick;
    if (tick_ < cooldownEnd)
    {
        return {SkillResult::Cooldown, cooldownEnd - tick_};
    }
    if (skill == SkillKind::Roll && !isUnitVector(direction))
    {
        return {SkillResult::InvalidDirection, 0};
    }

    cooldownEnd = tick_ + (skill == SkillKind::Roll ? kRollCooldownTicks : kSpinCooldownTicks);
    for (const auto& [viewerId, viewer] : players_)
    {
        if (viewerId == id || viewer->visible.contains(id))
        {
            observer_.onSkillUsed(viewerId, {id, skill, cooldownEnd, direction, tick_});
        }
    }

    if (skill == SkillKind::Roll)
    {
        player.rollTicksLeft = kRollTicks;
        player.rollDirection = direction;
    }
    else
    {
        spinKick(player);
    }
    return {SkillResult::Accepted, 0};
}

void Zone::spinKick(Player& player)
{
    std::vector<std::pair<float, Slime*>> targets;
    for (const auto& [id, slime] : slimes_)
    {
        const float distance = distanceBetween(player.position, slime->position);
        if (slime->mode != SlimeMode::Dead && slime->mode != SlimeMode::Return && distance <= kSpinRadius)
        {
            targets.emplace_back(distance, slime.get());
        }
    }

    std::sort(targets.begin(), targets.end(),
              [](const auto& left, const auto& right) { return left.first < right.first; });
    if (targets.size() > kSpinMaxTargets)
    {
        targets.resize(kSpinMaxTargets);
    }

    for (const auto& [distance, slime] : targets)
    {
        if (hitSlime(player, *slime, kSpinCoefficient, SkillKind::SpinKick))
        {
            // 재기절은 남은 시간을 새 시간으로 덮어쓴다. 끝 틱은 이번 틱 다음부터 센다.
            slime->stunEndTick = tick_ + kStunTicks + 1;
        }
    }
}

void Zone::updateRoll(Player& player)
{
    const Vec2 start = player.position;
    const float distance = distanceToBoundary(start, player.rollDirection, kRollStepPerTick);
    player.position.x += player.rollDirection.x * distance;
    player.position.y += player.rollDirection.y * distance;
    // 부동소수점 오차로 경계를 넘지 않게 한다.
    player.position.x = std::clamp(player.position.x, 0.0f, kMapSize);
    player.position.y = std::clamp(player.position.y, 0.0f, kMapSize);
    player.rolled = true;

    for (const auto& [id, slime] : slimes_)
    {
        if (slime->mode == SlimeMode::Dead || slime->mode == SlimeMode::Return || player.rollHits.contains(id)
            || distanceToSegment(slime->position, start, player.position) > kRollHalfWidth)
        {
            continue;
        }
        player.rollHits.insert(id);
        hitSlime(player, *slime, kRollCoefficient, SkillKind::Roll);
    }

    if (--player.rollTicksLeft == 0)
    {
        finishRoll(player);
    }
}

void Zone::finishRoll(Player& player)
{
    player.rollHits.clear();
    if (player.pendingDestination)
    {
        player.destination = player.pendingDestination;
        player.pendingDestination.reset();
    }
    else if (player.pendingTarget)
    {
        // 구르는 동안 대상이 죽었거나 복귀를 시작했으면 지정은 사라진다.
        const auto found = slimes_.find(*player.pendingTarget);
        if (found != slimes_.end() && found->second->mode != SlimeMode::Dead
            && found->second->mode != SlimeMode::Return)
        {
            player.target = player.pendingTarget;
            player.destination.reset();
        }
        player.pendingTarget.reset();
    }
}
}
