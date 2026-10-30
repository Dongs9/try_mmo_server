#pragma once

#include "Zone.h"

#include <cstddef>

// 기획서 수치를 서버 틱 단위로 옮긴 값. 10틱이 1초다.
namespace trymmo::balance
{
inline constexpr float kMapSize = 1600.0f;
inline constexpr Vec2 kEnterPosition{800.0f, 800.0f};
inline constexpr std::size_t kMaxPlayers = 100;
inline constexpr float kAoiEnterDistance = 700.0f;
inline constexpr float kAoiLeaveDistance = 800.0f;

inline constexpr int kPlayerMaxHp = 100;
inline constexpr int kPlayerStartLevel = 1;
inline constexpr int kPlayerAttack = 48;
inline constexpr int kPlayerDefense = 20;
inline constexpr float kPlayerStepPerTick = 10.0f;  // 초당 100
inline constexpr int kPlayerAttackPeriodTicks = 10;
inline constexpr int kPlayerRegenPeriodTicks = 10;
inline constexpr int kPlayerReviveTicks = 30;

inline constexpr float kAttackRange = 60.0f;
inline constexpr float kBasicAttackCoefficient = 1.0f;
inline constexpr float kDamageRandomMin = 0.98f;
inline constexpr float kDamageRandomMax = 1.02f;

inline constexpr int kSlimeCount = 30;
inline constexpr int kSlimeMaxHp = 120;
inline constexpr int kSlimeLevel = 1;
inline constexpr int kSlimeAttack = 5;
inline constexpr int kSlimeDefense = 5;
inline constexpr float kSlimeSpawnRadius = 600.0f;
inline constexpr float kSlimeSafeRadius = 100.0f;  // 입장 위치 주변은 생성 구역에서 뺀다
inline constexpr float kSlimeWanderRadius = 100.0f;
inline constexpr float kSlimeWanderStepPerTick = 4.0f;  // 초당 40
inline constexpr float kSlimeChaseStepPerTick = 8.0f;   // 초당 80
inline constexpr float kSlimeReturnStepPerTick = 16.0f;  // 초당 160
inline constexpr float kSlimeLeashDistance = 500.0f;
inline constexpr int kSlimeWanderMinTicks = 30;
inline constexpr int kSlimeWanderMaxTicks = 60;
inline constexpr int kSlimeAttackPeriodTicks = 20;
inline constexpr int kSlimeRespawnTicks = 300;
}
