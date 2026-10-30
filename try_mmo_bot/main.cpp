// 실제 메시지로 서버를 시험하는 봇. P1은 접속, 이동, 동기화, AOI 규칙을 시나리오로 확인한다.
#include "Packet.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>

namespace
{
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using std::chrono::duration;
using std::chrono::duration_cast;
using std::chrono::milliseconds;
namespace proto = trymmo::proto;

struct SeenPosition
{
    float x = 0.0f;
    float y = 0.0f;
    proto::EntityState state = proto::ENTITY_STATE_UNSPECIFIED;
    int hp = 0;
    int level = 0;
};

struct Despawn
{
    std::uint32_t entity = 0;
    proto::DespawnReason reason = proto::DESPAWN_REASON_UNSPECIFIED;
    std::uint32_t tick = 0;
    int sequence = 0;
};

struct SpawnRecord
{
    proto::EntityType type = proto::ENTITY_TYPE_UNSPECIFIED;
    float x = 0.0f;
    float y = 0.0f;
    std::uint32_t tick = 0;
};

struct Hit
{
    std::uint32_t attacker = 0;
    std::uint32_t target = 0;
    int damage = 0;
    int targetHp = 0;
    std::uint32_t tick = 0;
    int sequence = 0;
    float distance = -1.0f;  // 직전 스냅샷 기준 두 개체의 거리
};

struct Death
{
    std::uint32_t entity = 0;
    std::uint32_t reviveTick = 0;
    std::uint32_t tick = 0;
};

float distanceBetween(float ax, float ay, float bx, float by)
{
    return std::hypot(ax - bx, ay - by);
}

class Client
{
public:
    explicit Client(std::uint16_t port) : socket_(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP))
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        if (connect(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
        {
            throw std::runtime_error("connect failed: " + std::to_string(WSAGetLastError()));
        }
        const BOOL noDelay = TRUE;
        setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    }

    ~Client() { closesocket(socket_); }

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void sendMessage(proto::MsgId msgId, const google::protobuf::MessageLite& message)
    {
        const auto frame = trymmo::encodeFrame(msgId, message);
        const auto length = static_cast<std::uint16_t>(frame.size());
        std::vector<char> bytes = {static_cast<char>(length & 0xFF), static_cast<char>(length >> 8)};
        bytes.insert(bytes.end(), reinterpret_cast<const char*>(frame.data()),
                     reinterpret_cast<const char*>(frame.data()) + frame.size());
        sendRaw(bytes);
    }

    void sendRaw(const std::vector<char>& bytes)
    {
        ::send(socket_, bytes.data(), static_cast<int>(bytes.size()), 0);
    }

    void enter() { sendMessage(proto::C2S_ENTER_REQ, proto::EnterReq{}); }

    void moveTo(float x, float y)
    {
        proto::MoveReq request;
        request.mutable_destination()->set_x(x);
        request.mutable_destination()->set_y(y);
        sendMessage(proto::C2S_MOVE_REQ, request);
    }

    void target(std::uint32_t entity)
    {
        proto::TargetReq request;
        request.set_target_id(entity);
        sendMessage(proto::C2S_TARGET_REQ, request);
    }

    // 주어진 시간 동안 받은 메시지를 상태에 반영한다.
    void pump(milliseconds time)
    {
        const auto end = Clock::now() + time;
        while (!closed_)
        {
            const auto left = duration_cast<milliseconds>(end - Clock::now());
            if (left <= 0ms)
            {
                return;
            }

            WSAPOLLFD descriptor{socket_, POLLRDNORM, 0};
            if (WSAPoll(&descriptor, 1, static_cast<INT>(left.count())) <= 0)
            {
                continue;
            }

            char buffer[4096];
            const int received = recv(socket_, buffer, sizeof(buffer), 0);
            if (received <= 0)
            {
                closed_ = true;
                return;
            }
            const auto* bytes = reinterpret_cast<const std::byte*>(buffer);
            inbox_.insert(inbox_.end(), bytes, bytes + received);
            extractFrames();
        }
    }

    template <typename Predicate>
    bool waitUntil(milliseconds timeout, Predicate predicate)
    {
        const auto end = Clock::now() + timeout;
        while (!predicate() && !closed_ && Clock::now() < end)
        {
            pump(10ms);
        }
        return predicate();
    }

    bool closed() const { return closed_; }
    proto::EnterResult enterResult() const { return enterResult_; }
    std::uint32_t selfId() const { return selfId_; }
    std::uint32_t lastTick() const { return lastTick_; }
    bool sees(std::uint32_t entity) const { return positions_.contains(entity); }
    bool spawned(std::uint32_t entity) const { return spawned_.contains(entity); }
    const SeenPosition& position(std::uint32_t entity) const { return positions_.at(entity); }
    const std::vector<SeenPosition>& history(std::uint32_t entity) { return history_[entity]; }
    const std::vector<Despawn>& despawns() const { return despawns_; }
    std::vector<Despawn> despawnsOf(std::uint32_t entity) const
    {
        std::vector<Despawn> result;
        std::copy_if(despawns_.begin(), despawns_.end(), std::back_inserter(result),
                     [&](const Despawn& despawn) { return despawn.entity == entity; });
        return result;
    }
    std::vector<Hit> hitsBy(std::uint32_t attacker, std::uint32_t target) const
    {
        std::vector<Hit> result;
        std::copy_if(hits_.begin(), hits_.end(), std::back_inserter(result),
                     [&](const Hit& hit) { return hit.attacker == attacker && hit.target == target; });
        return result;
    }
    const std::vector<proto::RejectReason>& rejects() const { return rejects_; }
    const std::vector<Death>& deaths() const { return deaths_; }
    const SpawnRecord& spawnRecord(std::uint32_t entity) const { return spawnRecords_.at(entity); }

    // 위치를 아는 슬라임 중 가장 가까운 것. 없으면 0.
    std::uint32_t nearestSlime(std::uint32_t except = 0) const
    {
        std::uint32_t best = 0;
        float bestDistance = 1e9f;
        if (!positions_.contains(selfId_))
        {
            return 0;
        }
        const SeenPosition& self = positions_.at(selfId_);
        for (const auto& [id, seen] : positions_)
        {
            const auto record = spawnRecords_.find(id);
            if (id == except || record == spawnRecords_.end() || record->second.type != proto::ENTITY_TYPE_SLIME)
            {
                continue;
            }
            const float distance = distanceBetween(self.x, self.y, seen.x, seen.y);
            if (distance < bestDistance && seen.state != proto::ENTITY_STATE_RETURNING)
            {
                best = id;
                bestDistance = distance;
            }
        }
        return best;
    }

private:
    void extractFrames()
    {
        std::size_t offset = 0;
        while (inbox_.size() - offset >= 2)
        {
            const std::size_t length = std::to_integer<std::size_t>(inbox_[offset])
                                     | (std::to_integer<std::size_t>(inbox_[offset + 1]) << 8);
            if (inbox_.size() - offset - 2 < length)
            {
                break;
            }
            handleFrame(std::span(inbox_).subspan(offset + 2, length));
            offset += 2 + length;
        }
        inbox_.erase(inbox_.begin(), inbox_.begin() + offset);
    }

    void handleFrame(std::span<const std::byte> frame)
    {
        const auto payload = frame.subspan(trymmo::kMsgIdSize);
        switch (trymmo::readMsgId(frame))
        {
        case proto::S2C_ENTER_RES:
        {
            proto::EnterRes message;
            trymmo::decodePayload(payload, message);
            enterResult_ = message.result();
            selfId_ = message.self().entity_id();
            break;
        }
        case proto::S2C_ENTITY_SPAWN:
        {
            proto::EntitySpawn message;
            trymmo::decodePayload(payload, message);
            spawned_[message.entity().entity_id()] = true;
            spawnRecords_[message.entity().entity_id()] = {message.entity().type(), message.entity().position().x(),
                                                           message.entity().position().y(), lastTick_};
            break;
        }
        case proto::S2C_ENTITY_DESPAWN:
        {
            proto::EntityDespawn message;
            trymmo::decodePayload(payload, message);
            spawned_.erase(message.entity_id());
            positions_.erase(message.entity_id());
            despawns_.push_back({message.entity_id(), message.reason(), lastTick_, ++sequence_});
            break;
        }
        case proto::S2C_POSITION_SYNC:
        {
            proto::PositionSync message;
            trymmo::decodePayload(payload, message);
            lastTick_ = message.server_tick();
            for (const proto::EntityPosition& entity : message.entities())
            {
                const SeenPosition seen{entity.position().x(), entity.position().y(), entity.state(),
                                        entity.hp(), entity.level()};
                positions_[entity.entity_id()] = seen;
                history_[entity.entity_id()].push_back(seen);
            }
            break;
        }
        case proto::S2C_COMBAT_RESULT:
        {
            proto::CombatResult message;
            trymmo::decodePayload(payload, message);
            float distance = -1.0f;
            if (positions_.contains(message.attacker_id()) && positions_.contains(message.target_id()))
            {
                const SeenPosition& attacker = positions_.at(message.attacker_id());
                const SeenPosition& target = positions_.at(message.target_id());
                distance = distanceBetween(attacker.x, attacker.y, target.x, target.y);
            }
            hits_.push_back({message.attacker_id(), message.target_id(), message.damage(), message.target_hp(),
                             lastTick_, ++sequence_, distance});
            break;
        }
        case proto::S2C_PLAYER_DEATH:
        {
            proto::PlayerDeath message;
            trymmo::decodePayload(payload, message);
            deaths_.push_back({message.entity_id(), message.revive_tick(), lastTick_});
            break;
        }
        case proto::S2C_REQUEST_REJECTED:
        {
            proto::RequestRejected message;
            trymmo::decodePayload(payload, message);
            rejects_.push_back(message.reason());
            break;
        }
        default:
            break;
        }
    }

    SOCKET socket_;
    bool closed_ = false;
    std::vector<std::byte> inbox_;
    proto::EnterResult enterResult_ = proto::ENTER_RESULT_UNSPECIFIED;
    std::uint32_t selfId_ = 0;
    std::uint32_t lastTick_ = 0;
    std::unordered_map<std::uint32_t, bool> spawned_;
    std::unordered_map<std::uint32_t, SeenPosition> positions_;
    std::unordered_map<std::uint32_t, std::vector<SeenPosition>> history_;
    std::vector<Despawn> despawns_;
    std::unordered_map<std::uint32_t, SpawnRecord> spawnRecords_;
    std::vector<Hit> hits_;
    std::vector<proto::RejectReason> rejects_;
    std::vector<Death> deaths_;
    int sequence_ = 0;
};

int g_failures = 0;

void check(bool condition, const std::string& label)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << label << std::endl;
    if (!condition)
    {
        ++g_failures;
    }
}

bool enterAndWait(Client& client)
{
    client.enter();
    return client.waitUntil(3s, [&] { return client.enterResult() != proto::ENTER_RESULT_UNSPECIFIED; });
}

void testEnterAndSpawn(std::uint16_t port)
{
    Client a(port);
    Client b(port);

    const auto sentAt = Clock::now();
    check(enterAndWait(a) && a.enterResult() == proto::ENTER_RESULT_OK, "first client entered");
    const auto rttMs = duration<double, std::milli>(Clock::now() - sentAt).count();
    std::cout << "[INFO] enter round trip " << rttMs << "ms\n";

    check(enterAndWait(b) && b.enterResult() == proto::ENTER_RESULT_OK, "second client entered");
    check(a.selfId() != b.selfId(), "entity ids differ");
    check(a.waitUntil(2s, [&] { return a.spawned(b.selfId()); }), "a sees b spawn");
    check(b.waitUntil(2s, [&] { return b.spawned(a.selfId()); }), "b sees a spawn");
    check(!a.spawned(a.selfId()), "no spawn for self");
    check(a.waitUntil(2s, [&] { return a.sees(a.selfId()); }), "sync contains self");
    check(a.position(a.selfId()).hp == 100 && a.position(a.selfId()).level == 1, "sync contains hp and level");
}

void testMove(std::uint16_t port)
{
    Client a(port);
    Client b(port);
    enterAndWait(a);
    enterAndWait(b);
    b.waitUntil(2s, [&] { return b.sees(a.selfId()); });

    a.moveTo(900.0f, 800.0f);
    const bool arrived = b.waitUntil(5s, [&] {
        if (!b.sees(a.selfId()))
        {
            return false;
        }
        const SeenPosition& seen = b.position(a.selfId());
        return seen.x == 900.0f && seen.y == 800.0f && seen.state == proto::ENTITY_STATE_IDLE;
    });
    check(arrived, "b sees a arrive exactly at destination");

    float maxStep = 0.0f;
    const auto& history = b.history(a.selfId());
    for (std::size_t i = 1; i < history.size(); ++i)
    {
        maxStep = std::max(maxStep, std::hypot(history[i].x - history[i - 1].x, history[i].y - history[i - 1].y));
    }
    std::cout << "[INFO] max step per sync " << maxStep << '\n';
    check(maxStep <= 10.01f, "step per tick is at most 10");

    a.moveTo(-1.0f, 800.0f);
    a.moveTo(1601.0f, 800.0f);
    a.moveTo(std::nanf(""), 800.0f);
    a.pump(500ms);
    check(a.position(a.selfId()).x == 900.0f, "out of map moves ignored");

    a.moveTo(950.0f, 800.0f);
    a.pump(200ms);
    a.moveTo(900.0f, 800.0f);
    check(a.waitUntil(3s, [&] { return a.position(a.selfId()).x == 900.0f
                                    && a.position(a.selfId()).state == proto::ENTITY_STATE_IDLE; }),
          "new move overrides destination");
}

void testTickRate(std::uint16_t port)
{
    Client a(port);
    enterAndWait(a);
    a.waitUntil(2s, [&] { return a.lastTick() != 0; });
    const std::uint32_t first = a.lastTick();
    a.pump(5s);
    const std::uint32_t ticks = a.lastTick() - first;
    std::cout << "[INFO] ticks in 5s " << ticks << '\n';
    check(ticks >= 49 && ticks <= 51, "tick rate is 10Hz");
}

void testAoi(std::uint16_t port)
{
    Client a(port);
    Client b(port);
    enterAndWait(a);
    enterAndWait(b);
    b.waitUntil(2s, [&] { return b.sees(a.selfId()); });

    // (800,800)에서 대각선 끝까지 약 1131이므로 이탈 거리 800을 넘는다.
    a.moveTo(1600.0f, 1600.0f);
    const bool left = b.waitUntil(15s, [&] { return !b.spawned(a.selfId()); });
    check(left, "a left aoi of b");
    check(!b.despawnsOf(a.selfId()).empty() && b.despawnsOf(a.selfId()).back().reason == proto::DESPAWN_REASON_OUT_OF_AOI,
          "reason is out of aoi");
    a.waitUntil(5s, [&] { return a.sees(a.selfId()) && a.position(a.selfId()).state == proto::ENTITY_STATE_IDLE; });
    const float distance = std::hypot(a.position(a.selfId()).x - 800.0f, a.position(a.selfId()).y - 800.0f);
    std::cout << "[INFO] a stopped at distance " << distance << '\n';

    a.moveTo(800.0f, 800.0f);
    check(b.waitUntil(15s, [&] { return b.sees(a.selfId()); }), "a re-entered aoi of b");
    const SeenPosition seen = b.position(a.selfId());
    const float enteredAt = std::hypot(seen.x - 800.0f, seen.y - 800.0f);
    std::cout << "[INFO] re-entered at distance " << enteredAt << '\n';
    check(enteredAt <= 700.0f, "enter distance is at most 700");
}

bool moveAndArrive(Client& client, float x, float y)
{
    client.moveTo(x, y);
    return client.waitUntil(30s, [&] {
        if (!client.sees(client.selfId()))
        {
            return false;
        }
        const SeenPosition& seen = client.position(client.selfId());
        return seen.x == x && seen.y == y && seen.state == proto::ENTITY_STATE_IDLE;
    });
}

void testAoiBoundary(std::uint16_t port)
{
    Client a(port);
    Client b(port);
    enterAndWait(a);
    enterAndWait(b);
    check(moveAndArrive(b, 300.0f, 800.0f), "b moved to boundary test position");
    b.waitUntil(2s, [&] { return b.spawned(a.selfId()); });

    // b는 x=300에 있으므로 a의 x에서 300을 뺀 값이 거리다.
    for (int round = 1; round <= 2; ++round)
    {
        const std::string prefix = "round " + std::to_string(round) + ": ";
        const std::size_t despawnsBefore = b.despawnsOf(a.selfId()).size();

        check(moveAndArrive(a, 1100.0f, 800.0f), prefix + "a arrived at distance 800");
        b.pump(300ms);
        check(b.spawned(a.selfId()), prefix + "still visible at distance 800");

        check(moveAndArrive(a, 1101.0f, 800.0f), prefix + "a arrived at distance 801");
        check(b.waitUntil(1s, [&] { return !b.spawned(a.selfId()); }), prefix + "left aoi at distance 801");

        check(moveAndArrive(a, 1001.0f, 800.0f), prefix + "a arrived at distance 701");
        b.pump(300ms);
        check(!b.spawned(a.selfId()), prefix + "still hidden at distance 701");

        check(moveAndArrive(a, 1000.0f, 800.0f), prefix + "a arrived at distance 700");
        check(b.waitUntil(1s, [&] { return b.spawned(a.selfId()); }), prefix + "entered aoi at distance 700");

        check(b.despawnsOf(a.selfId()).size() == despawnsBefore + 1, prefix + "exactly one despawn");
    }
}

void testDisconnect(std::uint16_t port)
{
    Client a(port);
    enterAndWait(a);
    std::uint32_t otherId = 0;
    {
        Client b(port);
        enterAndWait(b);
        otherId = b.selfId();
        a.waitUntil(2s, [&] { return a.sees(otherId); });
    }
    check(a.waitUntil(2s, [&] { return !a.despawnsOf(otherId).empty(); }), "disconnect despawns");
    check(!a.despawnsOf(otherId).empty() && a.despawnsOf(otherId).back().reason == proto::DESPAWN_REASON_LEFT,
          "reason is left");
}

void testBadInput(std::uint16_t port)
{
    Client notEntered(port);
    notEntered.moveTo(100.0f, 100.0f);
    notEntered.pump(300ms);
    check(!notEntered.closed(), "move before enter keeps connection");

    Client unknown(port);
    unknown.sendRaw({2, 0, 99, 0});
    unknown.pump(300ms);
    check(!unknown.closed(), "unknown message keeps connection");

    Client zeroLength(port);
    zeroLength.sendRaw({0, 0});
    zeroLength.pump(500ms);
    check(zeroLength.closed(), "zero length frame closes connection");

    Client tooLong(port);
    tooLong.sendRaw({1, 0x20});  // 8193
    tooLong.pump(500ms);
    check(tooLong.closed(), "oversized frame closes connection");

    Client malformed(port);
    enterAndWait(malformed);
    malformed.sendRaw({3, 0, static_cast<char>(0xD0), 0x07, static_cast<char>(0xFF)});  // C2S_MOVE_REQ
    malformed.pump(500ms);
    check(malformed.closed(), "malformed move closes connection");

    // 한 번에 보낸 [잘못된 이동][입장 요청]. 닫는 연결은 응답을 보내지 않으므로
    // 뒤 프레임이 처리되지 않았는지는 서버 로그에 입장 기록이 없는 것으로 확인한다.
    Client afterClose(port);
    afterClose.sendRaw({3, 0, static_cast<char>(0xD0), 0x07, static_cast<char>(0xFF),
                        2, 0, static_cast<char>(0xE8), 0x03});
    afterClose.pump(500ms);
    check(afterClose.closed(), "malformed frame closes connection in burst");
}

void testRequestLimit(std::uint16_t port)
{
    Client a(port);
    enterAndWait(a);
    a.waitUntil(2s, [&] { return a.sees(a.selfId()); });
    a.pump(1100ms);  // 입장 요청이 든 구간을 지나보낸다

    // 25개를 한 번에 보내면 앞의 20개만 처리되어 마지막 목적지는 820이다.
    for (int i = 1; i <= 25; ++i)
    {
        a.moveTo(800.0f + static_cast<float>(i), 800.0f);
    }
    a.pump(3s);
    std::cout << "[INFO] x after burst " << a.position(a.selfId()).x << '\n';
    check(a.position(a.selfId()).x == 820.0f, "requests over 20 per second ignored");
    check(!a.closed(), "request limit keeps connection");

    a.moveTo(830.0f, 800.0f);
    check(a.waitUntil(3s, [&] { return a.position(a.selfId()).x == 830.0f; }), "requests accepted in next window");
}

void stopWhereYouAre(Client& client)
{
    const SeenPosition& self = client.position(client.selfId());
    client.moveTo(self.x, self.y);
}

// 슬라임이 보일 때까지 기다리고 가장 가까운 슬라임을 돌려준다.
std::uint32_t waitForSlime(Client& client, std::uint32_t except = 0)
{
    client.waitUntil(5s, [&] { return client.nearestSlime(except) != 0; });
    return client.nearestSlime(except);
}

void testSlimeSpawn(std::uint16_t port)
{
    Client a(port);
    enterAndWait(a);
    const std::uint32_t slime = waitForSlime(a);
    check(slime != 0, "slimes are visible after enter");
    check(a.position(slime).hp == 120, "slime starts with 120 hp");

    const SeenPosition before = a.position(slime);
    a.pump(7s);
    const SeenPosition after = a.position(slime);
    check(before.x != after.x || before.y != after.y, "slime wanders");
    check(after.hp == 120, "wandering slime keeps hp");
}

void testKillSlime(std::uint16_t port)
{
    Client a(port);
    Client b(port);
    enterAndWait(a);
    enterAndWait(b);
    const std::uint32_t slime = waitForSlime(a);
    b.waitUntil(3s, [&] { return b.sees(slime); });
    const SeenPosition start = a.position(slime);
    const SeenPosition selfStart = a.position(a.selfId());
    std::cout << "[INFO] target distance " << distanceBetween(selfStart.x, selfStart.y, start.x, start.y) << '\n';

    a.target(slime);
    check(a.waitUntil(40s, [&] { return !a.despawnsOf(slime).empty(); }), "slime killed");
    b.waitUntil(3s, [&] { return !b.despawnsOf(slime).empty(); });

    const auto hits = a.hitsBy(a.selfId(), slime);
    const auto seenByB = b.hitsBy(a.selfId(), slime);
    check(hits.size() == 3, "slime dies in 3 hits");

    bool damageInRange = !hits.empty();
    bool hpMatches = !hits.empty();
    int hp = 120;
    for (const Hit& hit : hits)
    {
        damageInRange = damageInRange && hit.damage >= 45 && hit.damage <= 47;
        hp = std::max(hp - hit.damage, 0);
        hpMatches = hpMatches && hit.targetHp == hp;
    }
    check(damageInRange, "damage is 45 to 47");
    check(hpMatches && hp == 0, "hp decreases to 0");

    // 타격 틱은 직전 스냅샷 기준이라 이동 1틱분(플레이어 10, 슬라임 4)만큼 멀 수 있다.
    std::cout << "[INFO] first hit distance " << (hits.empty() ? -1.0f : hits.front().distance) << '\n';
    check(!hits.empty() && hits.front().distance >= 0.0f && hits.front().distance <= 75.0f, "attacks from within range");

    bool attackPeriod = hits.size() == 3;
    for (std::size_t i = 1; i < hits.size(); ++i)
    {
        const std::uint32_t gap = hits[i].tick - hits[i - 1].tick;
        std::cout << "[INFO] attack gap ticks " << gap << '\n';
        attackPeriod = attackPeriod && gap >= 9 && gap <= 11;
    }
    check(attackPeriod, "attack period is 1 second");

    bool sameOnBothScreens = seenByB.size() == hits.size() && !hits.empty();
    for (std::size_t i = 0; sameOnBothScreens && i < hits.size(); ++i)
    {
        sameOnBothScreens = hits[i].damage == seenByB[i].damage && hits[i].targetHp == seenByB[i].targetHp;
    }
    check(sameOnBothScreens, "both clients see the same hp");

    const auto despawns = a.despawnsOf(slime);
    check(!despawns.empty() && despawns.back().reason == proto::DESPAWN_REASON_DIED, "despawn reason is died");
    check(!despawns.empty() && !hits.empty() && hits.back().sequence < despawns.back().sequence,
          "combat result comes before despawn");

    a.target(slime);
    check(a.waitUntil(2s, [&] { return !a.rejects().empty(); })
              && a.rejects().back() == proto::REJECT_REASON_TARGET_DEAD, "dead target rejected");

    a.target(999999);
    check(a.waitUntil(2s, [&] { return a.rejects().size() >= 2; })
              && a.rejects().back() == proto::REJECT_REASON_TARGET_NOT_FOUND, "unknown target rejected");

    a.target(b.selfId());
    check(a.waitUntil(2s, [&] { return a.rejects().size() >= 3; })
              && a.rejects().back() == proto::REJECT_REASON_TARGET_NOT_FOUND, "player target rejected");

    const std::uint32_t other = waitForSlime(a, slime);
    a.target(other);
    stopWhereYouAre(a);
    a.pump(2500ms);
    check(a.hitsBy(a.selfId(), other).empty(), "move input clears target");

    // 위치가 바뀐 스냅샷은 마지막 이동 틱까지 MOVING이어야 한다.
    bool movingWhenMoved = true;
    const auto& selfHistory = a.history(a.selfId());
    for (std::size_t i = 1; i < selfHistory.size(); ++i)
    {
        const bool moved = selfHistory[i].x != selfHistory[i - 1].x || selfHistory[i].y != selfHistory[i - 1].y;
        movingWhenMoved = movingWhenMoved && (!moved || selfHistory[i].state == proto::ENTITY_STATE_MOVING);
    }
    check(selfHistory.size() > 1 && movingWhenMoved, "state is moving on every tick that moved");

    const std::uint32_t deadTick = despawns.empty() ? 0 : despawns.back().tick;
    check(a.waitUntil(35s, [&] { return a.spawned(slime) && a.sees(slime); }), "slime respawned");
    if (a.sees(slime))
    {
        const SpawnRecord& record = a.spawnRecord(slime);
        const std::uint32_t waited = record.tick - deadTick;
        const float moved = distanceBetween(start.x, start.y, record.x, record.y);
        std::cout << "[INFO] respawn after ticks " << waited << ", distance from first seen " << moved << '\n';
        check(waited >= 299 && waited <= 301, "respawn takes 30 seconds");
        check(moved <= 100.5f, "respawn at spawn point");
        check(a.position(slime).hp == 120, "respawned slime has full hp");
    }
}

void testChaseAndReturn(std::uint16_t port)
{
    Client a(port);
    enterAndWait(a);
    const std::uint32_t slime = waitForSlime(a);
    const SeenPosition start = a.position(slime);

    a.target(slime);
    a.waitUntil(30s, [&] { return !a.hitsBy(a.selfId(), slime).empty(); });
    stopWhereYouAre(a);
    const auto firstHits = a.hitsBy(a.selfId(), slime);
    check(firstHits.size() == 1, "one hit before stopping");
    const int slimeHp = firstHits.empty() ? 0 : firstHits.front().targetHp;

    check(a.waitUntil(8s, [&] { return a.hitsBy(slime, a.selfId()).size() >= 2; }), "slime attacks its attacker");
    const auto counter = a.hitsBy(slime, a.selfId());
    if (counter.size() >= 2)
    {
        std::cout << "[INFO] slime attack gap ticks " << counter[1].tick - counter[0].tick << '\n';
        check(counter[0].damage == 4, "slime deals 4 damage");
        check(counter[1].tick - counter[0].tick >= 19 && counter[1].tick - counter[0].tick <= 21,
              "slime attack period is 2 seconds");
    }
    check(a.position(slime).hp == slimeHp, "slime does not recover during combat");

    const int lowestHp = counter.empty() ? 0 : counter.back().targetHp;
    check(a.waitUntil(2s, [&] { return a.position(a.selfId()).hp > lowestHp; }), "player regenerates during combat");

    // 슬라임에게서 가장 먼 모서리로 달려 생성 지점 500 밖으로 끌어낸다.
    const float cornerX = start.x < 800.0f ? 1600.0f : 0.0f;
    const float cornerY = start.y < 800.0f ? 1600.0f : 0.0f;
    a.moveTo(cornerX, cornerY);
    check(a.waitUntil(20s, [&] { return a.sees(slime) && a.position(slime).state == proto::ENTITY_STATE_RETURNING; }),
          "slime returns beyond leash distance");
    if (a.sees(slime))
    {
        const SeenPosition& seen = a.position(slime);
        const float leash = distanceBetween(start.x, start.y, seen.x, seen.y);
        std::cout << "[INFO] returning at distance from first seen " << leash << '\n';
        check(leash >= 390.0f, "leash distance is about 500");
    }

    const std::size_t hitsBefore = a.hitsBy(a.selfId(), slime).size();
    a.target(slime);
    check(a.waitUntil(2s, [&] { return !a.rejects().empty(); })
              && a.rejects().back() == proto::REJECT_REASON_TARGET_RETURNING, "returning target rejected");
    stopWhereYouAre(a);
    a.pump(500ms);
    check(a.hitsBy(a.selfId(), slime).size() == hitsBefore, "returning slime takes no damage");

    check(a.waitUntil(10s, [&] {
              return a.sees(slime) && a.position(slime).state != proto::ENTITY_STATE_RETURNING
                  && a.position(slime).hp == 120;
          }), "slime recovers after return");
}

void testChaseTargetLeaves(std::uint16_t port)
{
    Client watcher(port);
    enterAndWait(watcher);
    std::uint32_t slime = 0;
    {
        Client a(port);
        enterAndWait(a);
        slime = waitForSlime(a);
        watcher.waitUntil(3s, [&] { return watcher.sees(slime); });
        a.target(slime);
        check(a.waitUntil(30s, [&] { return !a.hitsBy(a.selfId(), slime).empty(); }), "slime hit before leaving");
    }
    check(watcher.waitUntil(8s, [&] {
              return watcher.sees(slime) && watcher.position(slime).state != proto::ENTITY_STATE_RETURNING
                  && watcher.position(slime).hp == 120;
          }), "slime recovers after attacker disconnects");
}

// 서버를 --debug-player-hp 6으로 띄워야 한다. 기본 HP로는 슬라임 한 마리에게 죽지 않는다.
void testPlayerDeath(std::uint16_t port)
{
    Client a(port);
    Client b(port);
    enterAndWait(a);
    enterAndWait(b);
    const std::uint32_t slime = waitForSlime(a);

    a.target(slime);
    check(a.waitUntil(30s, [&] { return !a.hitsBy(a.selfId(), slime).empty(); }), "hit a slime");
    stopWhereYouAre(a);

    check(a.waitUntil(30s, [&] { return !a.deaths().empty(); }), "player died");
    check(b.waitUntil(3s, [&] { return !b.deaths().empty(); }), "other player sees death");
    if (a.deaths().empty())
    {
        return;
    }
    const Death death = a.deaths().back();
    std::cout << "[INFO] revive in ticks " << death.reviveTick - death.tick << '\n';
    check(death.entity == a.selfId(), "death is for self");
    check(death.reviveTick - death.tick >= 29 && death.reviveTick - death.tick <= 31, "revive tick is 3 seconds later");

    check(a.waitUntil(1s, [&] { return a.position(a.selfId()).state == proto::ENTITY_STATE_DEAD; }), "state is dead");
    check(a.position(a.selfId()).hp == 0, "hp is 0 while dead");

    const SeenPosition dead = a.position(a.selfId());
    const std::size_t rejectsBefore = a.rejects().size();
    a.moveTo(100.0f, 100.0f);
    a.target(slime);
    a.pump(1s);
    check(a.position(a.selfId()).x == dead.x && a.position(a.selfId()).y == dead.y, "move ignored while dead");
    check(a.rejects().size() == rejectsBefore, "input while dead gets no reject");

    check(a.waitUntil(5s, [&] { return a.position(a.selfId()).state != proto::ENTITY_STATE_DEAD; }), "player revived");
    const SeenPosition revived = a.position(a.selfId());
    std::cout << "[INFO] revived hp " << revived.hp << '\n';
    check(revived.x == 800.0f && revived.y == 800.0f, "revive at enter position");
    check(revived.hp == 6, "hp is full after revive");

    check(a.waitUntil(10s, [&] { return !a.sees(slime) || a.position(slime).hp == 120; }), "slime gives up after death");

    check(moveAndArrive(a, 820.0f, 800.0f), "input works after revive");
}

void testZoneFull(std::uint16_t port)
{
    std::vector<std::unique_ptr<Client>> clients;
    bool allEntered = true;
    for (int i = 0; i < 100; ++i)
    {
        clients.push_back(std::make_unique<Client>(port));
        allEntered = enterAndWait(*clients.back()) && clients.back()->enterResult() == proto::ENTER_RESULT_OK && allEntered;

        // 읽지 않는 클라이언트는 서버가 outbox 한도로 끊으므로 입장 중에도 수신을 비운다.
        if (i % 5 == 0)
        {
            for (const auto& client : clients)
            {
                client->pump(2ms);
            }
        }
    }
    check(allEntered, "100 clients entered");

    Client extra(port);
    check(enterAndWait(extra) && extra.enterResult() == proto::ENTER_RESULT_ZONE_FULL, "101st client rejected");
    extra.pump(300ms);
    check(!extra.closed(), "rejected client stays connected");
}
}

int main(int argc, char* argv[])
{
    try
    {
        const auto port = static_cast<std::uint16_t>(argc > 1 ? std::stoi(argv[1]) : 7777);

        WSADATA wsaData{};
        WSAStartup(MAKEWORD(2, 2), &wsaData);

        if (argc > 2 && std::string(argv[2]) == "death")
        {
            testPlayerDeath(port);
            WSACleanup();
            std::cout << (g_failures == 0 ? "ALL PASSED\n" : "FAILED: " + std::to_string(g_failures) + "\n");
            return g_failures == 0 ? 0 : 1;
        }

        testEnterAndSpawn(port);
        testMove(port);
        testTickRate(port);
        testAoi(port);
        testAoiBoundary(port);
        testDisconnect(port);
        testBadInput(port);
        testRequestLimit(port);
        testSlimeSpawn(port);
        testKillSlime(port);
        testChaseAndReturn(port);
        testChaseTargetLeaves(port);
        testZoneFull(port);

        WSACleanup();
    }
    catch (const std::exception& error)
    {
        std::cerr << "fatal: " << error.what() << '\n';
        return 2;
    }

    std::cout << (g_failures == 0 ? "ALL PASSED\n" : "FAILED: " + std::to_string(g_failures) + "\n");
    return g_failures == 0 ? 0 : 1;
}
