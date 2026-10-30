#pragma once

#include "Zone.h"
#include "../net/Transport.h"

#include <atomic>
#include <chrono>
#include <optional>
#include <unordered_map>

namespace trymmo
{
// 접속 하나의 상태. 입장 전에는 플레이어가 없다.
struct Session
{
    std::optional<EntityId> player;
    std::chrono::steady_clock::time_point requestWindowStart;
    int requestCount = 0;
};

// 전송 계층과 존 사이의 다리. 프레임을 메시지로 풀어 존에 전달하고,
// 존의 사건을 메시지로 만들어 해당 세션에 보낸다. 틱 루프도 여기 있다.
class GameServer final : private ITransportHandler, private IZoneObserver
{
public:
    GameServer(ITransport& transport, ZoneOptions zoneOptions);

    // stop이 true가 될 때까지 돌아간다.
    void run(const std::atomic<bool>& stop);

private:
    void onConnected(ConnectionId connection) override;
    void onDisconnected(ConnectionId connection) override;
    void onFrame(ConnectionId connection, std::span<const std::byte> frame) override;

    void onEntitySpawn(EntityId viewer, const EntityView& entity) override;
    void onEntityDespawn(EntityId viewer, EntityId entity, DespawnReason reason) override;
    void onPositionSync(EntityId viewer, std::uint32_t tick, const std::vector<EntitySnapshot>& entities) override;
    void onCombat(EntityId viewer, const CombatEvent& event) override;
    void onPlayerDeath(EntityId viewer, EntityId player, std::uint32_t reviveTick) override;

    bool allowRequest(ConnectionId connection, Session& session);
    void handleEnter(ConnectionId connection, Session& session);
    void handleMove(ConnectionId connection, Session& session, std::span<const std::byte> payload);
    void handleTarget(ConnectionId connection, Session& session, std::span<const std::byte> payload);

    ITransport& transport_;
    Zone zone_;
    std::unordered_map<ConnectionId, Session> sessions_;
    std::unordered_map<EntityId, ConnectionId> playerConnections_;
};
}
