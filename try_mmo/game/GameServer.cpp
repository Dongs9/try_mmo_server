#include "GameServer.h"

#include "Packet.h"
#include "../Log.h"

#include <algorithm>
#include <chrono>
#include <string>

namespace trymmo
{
namespace
{
using Clock = std::chrono::steady_clock;
using std::chrono::ceil;
using std::chrono::duration_cast;
using std::chrono::microseconds;
using std::chrono::milliseconds;

constexpr milliseconds kTickInterval{100};
// 이만큼 밀리면 따라잡기를 포기한다. 몰아서 돌리면 틱 간격이 더 어긋난다.
constexpr int kMaxCatchUpTicks = 5;
constexpr std::chrono::seconds kRequestWindow{1};
constexpr int kMaxRequestsPerWindow = 20;

proto::Vec2 toMessage(Vec2 value)
{
    proto::Vec2 message;
    message.set_x(value.x);
    message.set_y(value.y);
    return message;
}

proto::EntityState toMessage(EntityState state)
{
    return state == EntityState::Moving ? proto::ENTITY_STATE_MOVING : proto::ENTITY_STATE_IDLE;
}

proto::EntityInfo toMessage(const EntityView& entity)
{
    proto::EntityInfo info;
    info.set_entity_id(entity.id);
    info.set_type(proto::ENTITY_TYPE_PLAYER);
    info.set_name(entity.name);
    info.set_max_hp(entity.maxHp);
    *info.mutable_position() = toMessage(entity.position);
    info.set_state(toMessage(entity.state));
    return info;
}
}

GameServer::GameServer(ITransport& transport)
    : transport_(transport), zone_(*this)
{
    transport_.setHandler(this);
}

void GameServer::run(const std::atomic<bool>& stop)
{
    // 단일 스레드 루프: 다음 틱까지 입출력을 처리하고, 시간이 되면 틱을 돌린다.
    auto nextTick = Clock::now() + kTickInterval;
    while (!stop.load(std::memory_order_relaxed))
    {
        const auto wait = ceil<milliseconds>(nextTick - Clock::now());
        transport_.poll(std::clamp(wait, milliseconds(0), kTickInterval));

        const auto now = Clock::now();
        if (now < nextTick)
        {
            continue;
        }

        zone_.tick();
        nextTick += kTickInterval;

        const auto tickCost = Clock::now() - now;
        if (tickCost >= kTickInterval)
        {
            logWarn("tick " + std::to_string(zone_.currentTick()) + " took "
                    + std::to_string(duration_cast<microseconds>(tickCost).count()) + "us");
        }
        if (Clock::now() - nextTick > kTickInterval * kMaxCatchUpTicks)
        {
            logWarn("server is behind, skipping missed ticks");
            nextTick = Clock::now() + kTickInterval;
        }
    }
}

void GameServer::onConnected(ConnectionId connection)
{
    sessions_.emplace(connection, Session{});
    logInfo("connection " + std::to_string(connection) + " opened");
}

void GameServer::onDisconnected(ConnectionId connection)
{
    const auto found = sessions_.find(connection);
    if (found == sessions_.end())
    {
        return;
    }

    if (found->second.player)
    {
        const EntityId player = *found->second.player;
        playerConnections_.erase(player);
        zone_.removePlayer(player);
    }
    sessions_.erase(found);
    logInfo("connection " + std::to_string(connection) + " closed");
}

void GameServer::onFrame(ConnectionId connection, std::span<const std::byte> frame)
{
    const auto found = sessions_.find(connection);
    if (found == sessions_.end())
    {
        return;
    }

    if (!allowRequest(connection, found->second))
    {
        return;
    }

    const proto::MsgId msgId = readMsgId(frame);
    const auto payload = frame.subspan(kMsgIdSize);
    switch (msgId)
    {
    case proto::C2S_ENTER_REQ:
        handleEnter(connection, found->second);
        break;
    case proto::C2S_MOVE_REQ:
        handleMove(connection, found->second, payload);
        break;
    default:
        logWarn("connection " + std::to_string(connection) + " sent unknown message " + std::to_string(msgId));
        break;
    }
}

void GameServer::onEntitySpawn(EntityId viewer, const EntityView& entity)
{
    proto::EntitySpawn message;
    *message.mutable_entity() = toMessage(entity);
    transport_.send(playerConnections_.at(viewer), encodeFrame(proto::S2C_ENTITY_SPAWN, message));
}

void GameServer::onEntityDespawn(EntityId viewer, EntityId entity, DespawnReason reason)
{
    proto::EntityDespawn message;
    message.set_entity_id(entity);
    message.set_reason(reason == DespawnReason::Left ? proto::DESPAWN_REASON_LEFT
                                                     : proto::DESPAWN_REASON_OUT_OF_AOI);
    transport_.send(playerConnections_.at(viewer), encodeFrame(proto::S2C_ENTITY_DESPAWN, message));
}

void GameServer::onPositionSync(EntityId viewer, std::uint32_t tick, const std::vector<EntitySnapshot>& entities)
{
    proto::PositionSync message;
    message.set_server_tick(tick);
    for (const EntitySnapshot& entity : entities)
    {
        proto::EntityPosition* position = message.add_entities();
        position->set_entity_id(entity.id);
        *position->mutable_position() = toMessage(entity.position);
        position->set_state(toMessage(entity.state));
        position->set_hp(entity.hp);
        position->set_level(entity.level);
    }
    transport_.send(playerConnections_.at(viewer), encodeFrame(proto::S2C_POSITION_SYNC, message));
}

bool GameServer::allowRequest(ConnectionId connection, Session& session)
{
    // 고정 1초 구간으로 센다. 구간의 첫 요청이 시작점이다.
    const auto now = Clock::now();
    if (now - session.requestWindowStart >= kRequestWindow)
    {
        session.requestWindowStart = now;
        session.requestCount = 0;
    }

    ++session.requestCount;
    if (session.requestCount <= kMaxRequestsPerWindow)
    {
        return true;
    }

    if (session.requestCount == kMaxRequestsPerWindow + 1)
    {
        logWarn("connection " + std::to_string(connection) + " exceeded request limit, ignoring until next window");
    }
    return false;
}

void GameServer::handleEnter(ConnectionId connection, Session& session)
{
    if (session.player)
    {
        logWarn("connection " + std::to_string(connection) + " sent duplicate enter");
        return;
    }

    proto::EnterRes response;
    response.set_server_tick(zone_.currentTick());
    if (const std::optional<EntityView> player = zone_.addPlayer())
    {
        session.player = player->id;
        playerConnections_.emplace(player->id, connection);
        response.set_result(proto::ENTER_RESULT_OK);
        *response.mutable_self() = toMessage(*player);
        logInfo("connection " + std::to_string(connection) + " entered as " + player->name);
    }
    else
    {
        response.set_result(proto::ENTER_RESULT_ZONE_FULL);
        logWarn("connection " + std::to_string(connection) + " rejected, zone full");
    }
    transport_.send(connection, encodeFrame(proto::S2C_ENTER_RES, response));
}

void GameServer::handleMove(ConnectionId connection, Session& session, std::span<const std::byte> payload)
{
    proto::MoveReq request;
    if (!decodePayload(payload, request) || !request.has_destination())
    {
        logWarn("connection " + std::to_string(connection) + " sent malformed move, closing");
        transport_.close(connection);
        return;
    }

    if (!session.player)
    {
        logWarn("connection " + std::to_string(connection) + " sent move before enter");
        return;
    }

    const Vec2 destination{request.destination().x(), request.destination().y()};
    if (!zone_.setDestination(*session.player, destination))
    {
        logWarn("player " + std::to_string(*session.player) + " move out of map ignored: ("
                + std::to_string(destination.x) + ", " + std::to_string(destination.y) + ")");
    }
}
}
