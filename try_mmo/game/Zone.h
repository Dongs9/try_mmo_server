#pragma once

#include <cstdint>
#include <memory>
#include <optional>
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

enum class EntityState : std::uint8_t
{
    Idle,
    Moving
};

enum class DespawnReason : std::uint8_t
{
    Left,
    OutOfAoi
};

struct EntityView
{
    EntityId id = 0;
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
};

// 존에서 일어난 일을 바깥으로 알린다. 존은 네트워크와 Protobuf를 모른다.
class IZoneObserver
{
public:
    virtual ~IZoneObserver() = default;

    virtual void onEntitySpawn(EntityId viewer, const EntityView& entity) = 0;
    virtual void onEntityDespawn(EntityId viewer, EntityId entity, DespawnReason reason) = 0;
    virtual void onPositionSync(EntityId viewer, std::uint32_t tick, const std::vector<EntitySnapshot>& entities) = 0;
};

// 맵 하나의 세계 상태. 틱마다 이동, AOI 갱신, 스냅샷 전달 순서로 진행한다.
class Zone
{
public:
    explicit Zone(IZoneObserver& observer) : observer_(observer) {}

    // 존이 가득 찼으면 nullopt를 돌려준다.
    std::optional<EntityView> addPlayer();
    void removePlayer(EntityId id);
    // 맵 밖 좌표면 false를 돌려주고 아무것도 바꾸지 않는다.
    bool setDestination(EntityId id, Vec2 destination);
    void tick();

    std::uint32_t currentTick() const { return tick_; }

private:
    struct Player
    {
        EntityId id = 0;
        std::string name;
        Vec2 position;
        int hp = 0;
        int level = 0;
        std::optional<Vec2> destination;
        std::unordered_set<EntityId> visible;  // 이 플레이어에게 등장을 알린 개체
    };

    void movePlayers();
    void updateVisibility();
    void sendPositions();

    static EntityView makeView(const Player& player);
    static EntitySnapshot makeSnapshot(const Player& player);

    IZoneObserver& observer_;
    std::unordered_map<EntityId, std::unique_ptr<Player>> players_;
    EntityId nextEntityId_ = 1;
    std::uint32_t tick_ = 0;
};
}
