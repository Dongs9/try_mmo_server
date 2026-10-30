#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace trymmo
{
using EntityId = std::uint32_t;

struct Vec2
{
    float x = 0.0f;
    float y = 0.0f;
};

enum class EntityType : std::uint8_t
{
    Player,
    Slime
};

enum class EntityState : std::uint8_t
{
    Idle,
    Moving,
    Returning,
    Dead
};

enum class DespawnReason : std::uint8_t
{
    Left,
    OutOfAoi,
    Died
};

enum class MoveResult : std::uint8_t
{
    Accepted,
    OutOfMap,
    PlayerDead
};

enum class TargetResult : std::uint8_t
{
    Accepted,
    NotFound,
    TargetDead,
    TargetReturning,
    PlayerDead
};

struct EntityView
{
    EntityId id = 0;
    EntityType type = EntityType::Player;
    std::string name;
    int maxHp = 0;
    Vec2 position;
    EntityState state = EntityState::Idle;
};

struct EntitySnapshot
{
    EntityId id = 0;
    Vec2 position;
    EntityState state = EntityState::Idle;
    int hp = 0;
    int level = 0;
    int maxHp = 0;
};

struct CombatEvent
{
    EntityId attacker = 0;
    EntityId target = 0;
    int damage = 0;
    int targetHp = 0;
};

struct PlayerProgress
{
    int level = 0;
    int exp = 0;
    int requiredExp = 0;  // 최고 레벨이면 0
    int gold = 0;
    int maxHp = 0;
    int attack = 0;
    int defense = 0;
};

struct RewardEvent
{
    EntityId slime = 0;
    Vec2 slimePosition;
    int expGained = 0;
    int goldGained = 0;
    int levelsGained = 0;
    PlayerProgress progress;
};

// 존에서 일어난 일을 바깥으로 알린다. 존은 네트워크와 Protobuf를 모른다.
class IZoneObserver
{
public:
    virtual ~IZoneObserver() = default;

    virtual void onEntitySpawn(EntityId viewer, const EntityView& entity) = 0;
    virtual void onEntityDespawn(EntityId viewer, EntityId entity, DespawnReason reason) = 0;
    virtual void onPositionSync(EntityId viewer, std::uint32_t tick, const std::vector<EntitySnapshot>& entities) = 0;
    virtual void onCombat(EntityId viewer, const CombatEvent& event) = 0;
    virtual void onPlayerDeath(EntityId viewer, EntityId player, std::uint32_t reviveTick) = 0;
    virtual void onReward(EntityId player, const RewardEvent& event) = 0;
};

struct ZoneOptions
{
    // 기본값은 기획서 수치다. 시험에서 재현하기 어려운 조건을 만들 때만 바꾼다.
    int playerMaxHp;
    int playerAttack;
    int slimeKillExp;
};

// 맵 하나의 세계 상태. 틱마다 플레이어, 슬라임, AOI, 스냅샷 순서로 진행한다.
// 전투와 슬라임 행동은 ZoneCombat.cpp에 있다.
class Zone
{
public:
    Zone(IZoneObserver& observer, ZoneOptions options);

    // 존이 가득 찼으면 nullopt를 돌려준다.
    std::optional<EntityView> addPlayer();
    void removePlayer(EntityId id);
    MoveResult setDestination(EntityId id, Vec2 destination);
    TargetResult setTarget(EntityId id, EntityId target);
    void tick();

    PlayerProgress progressOf(EntityId id) const;
    std::uint32_t currentTick() const { return tick_; }

private:
    struct Player
    {
        EntityId id = 0;
        std::string name;
        Vec2 position;
        int hp = 0;
        int maxHp = 0;
        int level = 0;
        int attack = 0;
        int defense = 0;
        int exp = 0;
        int gold = 0;
        std::optional<Vec2> destination;
        std::optional<EntityId> target;
        bool moved = false;  // 이번 틱에 위치가 바뀌었는지. 상태는 이 값으로 정한다
        std::uint32_t nextAttackTick = 0;
        std::uint32_t nextRegenTick = 0;
        std::optional<std::uint32_t> reviveTick;  // 값이 있으면 사망 중
        std::unordered_set<EntityId> visible;     // 이 플레이어에게 등장을 알린 개체
    };

    enum class SlimeMode : std::uint8_t
    {
        Wander,
        Chase,
        Return,
        Dead
    };

    struct Slime
    {
        EntityId id = 0;
        Vec2 spawnPoint;
        Vec2 position;
        int hp = 0;
        SlimeMode mode = SlimeMode::Wander;
        std::optional<Vec2> destination;
        std::uint32_t nextWanderTick = 0;
        EntityId chaseTarget = 0;
        bool moved = false;
        std::uint32_t nextAttackTick = 0;
        std::uint32_t respawnTick = 0;
        std::unordered_map<EntityId, int> contributions;  // 플레이어별 누적 피해
    };

    // Zone.cpp: 세계 상태와 AOI
    void spawnSlimes();
    Vec2 randomSlimeSpawnPoint();
    Vec2 randomPointNear(Vec2 center, float radius);
    void updateVisibility();
    void updateVisibilityOf(Player& viewer, EntityId other, Vec2 otherPosition, const EntityView& view);
    void sendPositions();

    EntitySnapshot makeSnapshot(EntityId id) const;
    static EntityView makeView(const Player& player);
    static EntityView makeView(const Slime& slime);
    static EntitySnapshot makeSnapshot(const Player& player);
    static EntitySnapshot makeSnapshot(const Slime& slime);
    static EntityState stateOf(const Player& player);
    static EntityState stateOf(const Slime& slime);

    // ZoneCombat.cpp: 플레이어 행동
    void updatePlayers();
    void updatePlayerRegen(Player& player);
    void updatePlayerAttack(Player& player);
    void revivePlayer(Player& player);
    void killPlayer(Player& player);

    // ZoneCombat.cpp: 슬라임 행동
    void updateSlimes();
    void updateWander(Slime& slime);
    void updateChase(Slime& slime);
    void updateReturn(Slime& slime);
    void respawnSlime(Slime& slime);
    void startReturn(Slime& slime);
    void killSlime(Slime& slime, EntityId killer);

    // ZoneCombat.cpp: 보상과 성장
    void grantRewards(Slime& slime, EntityId killer);
    int gainExp(Player& player, int exp);
    void levelUp(Player& player);

    // ZoneCombat.cpp: 공통
    int rollDamage(int attack, float coefficient, int defense);
    void clearTargetsOn(EntityId slime);
    void broadcastCombat(const CombatEvent& event);
    std::uint32_t randomWanderDelay();

    IZoneObserver& observer_;
    ZoneOptions options_;
    std::unordered_map<EntityId, std::unique_ptr<Player>> players_;
    std::unordered_map<EntityId, std::unique_ptr<Slime>> slimes_;
    EntityId nextEntityId_ = 1;
    std::uint32_t nextPlayerNumber_ = 1;
    std::uint32_t tick_ = 0;
    std::mt19937 random_;
};

float distanceBetween(Vec2 a, Vec2 b);
// 도착하면 true. 남은 거리가 이동량 이하이면 목적지에 정확히 놓는다.
bool moveToward(Vec2& position, Vec2 destination, float step);
}
