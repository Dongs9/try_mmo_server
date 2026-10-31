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
    int maxHp = 0;
    std::uint32_t tick = 0;
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
    proto::SkillId skill = proto::SKILL_ID_UNSPECIFIED;
};

struct SkillRecord
{
    std::uint32_t caster = 0;
    proto::SkillId skill = proto::SKILL_ID_UNSPECIFIED;
    std::uint32_t cooldownEndTick = 0;
    std::uint32_t tick = 0;
};

struct RejectRecord
{
    proto::RejectReason reason = proto::REJECT_REASON_UNSPECIFIED;
    proto::SkillId skill = proto::SKILL_ID_UNSPECIFIED;
    std::uint32_t remainingMs = 0;
};

struct Reward
{
    std::uint32_t slime = 0;
    int expGained = 0;
    int goldGained = 0;
    int levelsGained = 0;
    proto::PlayerStats stats;
    float x = 0.0f;
    float y = 0.0f;
    std::uint32_t tick = 0;
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

    void skill(proto::SkillId id, float x = 0.0f, float y = 0.0f)
    {
        proto::SkillReq request;
        request.set_skill(id);
        request.mutable_direction()->set_x(x);
        request.mutable_direction()->set_y(y);
        sendMessage(proto::C2S_SKILL_REQ, request);
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

    // 대기 없이 소켓이 빌 때까지 읽고 버린다. 많은 클라이언트를 유지할 때 쓴다.
    void drain()
    {
        char buffer[65536];
        WSAPOLLFD descriptor{socket_, POLLRDNORM, 0};
        while (!closed_ && WSAPoll(&descriptor, 1, 0) > 0)
        {
            if (recv(socket_, buffer, sizeof(buffer), 0) <= 0)
            {
                closed_ = true;
            }
        }
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
    const std::vector<Reward>& rewards() const { return rewards_; }
    const std::vector<SkillRecord>& skills() const { return skills_; }
    const std::vector<RejectRecord>& rejectRecords() const { return rejectRecords_; }
    const std::vector<Hit>& allHits() const { return hits_; }
    const proto::PlayerStats& enterStats() const { return enterStats_; }
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
            enterStats_ = message.stats();
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
                                        entity.hp(), entity.level(), entity.max_hp(), message.server_tick()};
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
                             lastTick_, ++sequence_, distance, message.skill()});
            break;
        }
        case proto::S2C_SKILL_USED:
        {
            proto::SkillUsed message;
            trymmo::decodePayload(payload, message);
            skills_.push_back({message.entity_id(), message.skill(), message.cooldown_end_tick(), message.server_tick()});
            break;
        }
        case proto::S2C_REWARD:
        {
            proto::Reward message;
            trymmo::decodePayload(payload, message);
            rewards_.push_back({message.slime_id(), message.exp_gained(), message.gold_gained(), message.levels_gained(),
                                message.stats(), message.slime_position().x(), message.slime_position().y(), lastTick_});
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
            rejectRecords_.push_back({message.reason(), message.skill(), message.cooldown_remaining_ms()});
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
    std::vector<Reward> rewards_;
    std::vector<SkillRecord> skills_;
    std::vector<RejectRecord> rejectRecords_;
    proto::PlayerStats enterStats_;
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

// 가장 가까운 슬라임을 잡고 그 id를 돌려준다.
std::uint32_t killNearestSlime(Client& client, std::uint32_t except = 0)
{
    const std::uint32_t slime = waitForSlime(client, except);
    client.target(slime);
    client.waitUntil(40s, [&] { return !client.despawnsOf(slime).empty(); });
    return slime;
}

bool waitForRewards(Client& client, std::size_t count)
{
    return client.waitUntil(5s, [&] { return client.rewards().size() >= count; });
}

void testLevelUp(std::uint16_t port)
{
    Client a(port);
    enterAndWait(a);
    const proto::PlayerStats& start = a.enterStats();
    check(start.level() == 1 && start.exp() == 0 && start.required_exp() == 20 && start.gold() == 0
              && start.max_hp() == 100 && start.attack() == 48 && start.defense() == 20,
          "enter response has start stats");

    int gold = 0;
    bool rewardsValid = true;
    for (std::size_t kill = 1; kill <= 4; ++kill)
    {
        const std::uint32_t slime = killNearestSlime(a);
        rewardsValid = rewardsValid && waitForRewards(a, kill);
        if (a.rewards().size() >= kill)
        {
            const Reward& reward = a.rewards()[kill - 1];
            gold += reward.goldGained;
            rewardsValid = rewardsValid && reward.slime == slime && reward.expGained == 5 && reward.goldGained >= 6
                        && reward.goldGained <= 10 && reward.stats.gold() == gold;
        }
    }
    check(rewardsValid && a.rewards().size() == 4, "each kill gives 5 exp and 6 to 10 gold");
    if (a.rewards().size() < 4)
    {
        return;
    }

    const Reward& beforeLevel = a.rewards()[2];
    check(beforeLevel.stats.level() == 1 && beforeLevel.stats.exp() == 15 && beforeLevel.levelsGained == 0,
          "15 exp stays at level 1");

    const proto::PlayerStats& stats = a.rewards().back().stats;
    std::cout << "[INFO] level " << stats.level() << " exp " << stats.exp() << "/" << stats.required_exp()
              << " gold " << stats.gold() << '\n';
    check(a.rewards().back().levelsGained == 1 && stats.level() == 2, "20 exp reaches level 2");
    check(stats.exp() == 0 && stats.required_exp() == 25, "level 2 needs 25 exp");
    check(stats.max_hp() == 103 && stats.attack() == 49 && stats.defense() == 21, "level up raises stats");

    check(a.waitUntil(1s, [&] {
              const SeenPosition& self = a.position(a.selfId());
              return self.level == 2 && self.maxHp == 103;
          }), "sync shows new level and max hp");
    check(a.position(a.selfId()).hp == 103, "level up restores hp");
}

void testSharedKill(std::uint16_t port)
{
    Client a(port);
    Client b(port);
    enterAndWait(a);
    enterAndWait(b);

    bool bothRewarded = true;
    bool goldDiffered = false;
    for (std::size_t kill = 1; kill <= 5; ++kill)
    {
        const std::uint32_t slime = waitForSlime(a);
        b.waitUntil(3s, [&] { return b.sees(slime); });
        a.target(slime);
        b.target(slime);
        a.waitUntil(40s, [&] { return !a.despawnsOf(slime).empty(); });
        bothRewarded = bothRewarded && waitForRewards(a, kill) && waitForRewards(b, kill);
        if (a.rewards().size() >= kill && b.rewards().size() >= kill)
        {
            const Reward& first = a.rewards()[kill - 1];
            const Reward& second = b.rewards()[kill - 1];
            bothRewarded = bothRewarded && first.expGained == 5 && second.expGained == 5;
            goldDiffered = goldDiffered || first.goldGained != second.goldGained;
        }
    }
    check(bothRewarded, "all contributors get full exp");
    check(goldDiffered, "gold is rolled per player");
    check(a.rewards().size() == 5 && b.rewards().size() == 5, "one reward per kill");
}

void testContributionReset(std::uint16_t port)
{
    Client a(port);
    Client b(port);
    enterAndWait(a);
    enterAndWait(b);
    const std::uint32_t slime = waitForSlime(a);
    const SeenPosition start = a.position(slime);
    b.waitUntil(3s, [&] { return b.sees(slime); });

    a.target(slime);
    check(a.waitUntil(30s, [&] { return !a.hitsBy(a.selfId(), slime).empty(); }), "a hit the slime");

    const float cornerX = start.x < 800.0f ? 1600.0f : 0.0f;
    const float cornerY = start.y < 800.0f ? 1600.0f : 0.0f;
    a.moveTo(cornerX, cornerY);
    check(a.waitUntil(20s, [&] {
              return !a.sees(slime) || a.position(slime).state == proto::ENTITY_STATE_RETURNING;
          }), "slime starts returning");
    stopWhereYouAre(a);

    // b의 수신 버퍼에는 피격 전 스냅샷이 남아 있다. 복귀를 본 뒤에야 HP 120을 믿을 수 있다.
    check(b.waitUntil(15s, [&] {
              return !b.sees(slime) || b.position(slime).state == proto::ENTITY_STATE_RETURNING;
          }), "b sees slime leave or return");
    check(b.waitUntil(15s, [&] {
              return b.sees(slime) && b.position(slime).hp == 120
                  && b.position(slime).state != proto::ENTITY_STATE_RETURNING;
          }), "slime finished returning");

    b.target(slime);
    check(b.waitUntil(30s, [&] { return !b.despawnsOf(slime).empty(); }), "b killed the slime");
    check(waitForRewards(b, 1), "b is rewarded");
    a.pump(1s);
    check(a.rewards().empty(), "contribution is cleared after return");
}

// 서버를 --debug-player-attack 10으로 띄워야 한다. 타격당 피해가 9~10이라 12 미만을 만들 수 있다.
void testContributionThreshold(std::uint16_t port)
{
    Client a(port);
    Client b(port);
    enterAndWait(a);
    enterAndWait(b);

    const std::uint32_t slime = waitForSlime(a);
    b.waitUntil(3s, [&] { return b.sees(slime); });
    a.target(slime);
    check(a.waitUntil(30s, [&] { return !a.hitsBy(a.selfId(), slime).empty(); }), "a hit once");
    stopWhereYouAre(a);
    const int low = a.hitsBy(a.selfId(), slime).front().damage;
    std::cout << "[INFO] low damage " << low << '\n';
    check(low < 12, "damage is below contribution");

    b.target(slime);
    check(b.waitUntil(40s, [&] { return !b.despawnsOf(slime).empty(); }), "b killed the slime");
    check(waitForRewards(b, 1), "main contributor is rewarded");
    a.pump(1s);
    check(a.rewards().empty(), "contribution below 12 gets no reward");

    // 마지막 타격은 누적 피해가 12 미만이어도 보상을 받는다.
    const std::uint32_t second = waitForSlime(a, slime);
    b.waitUntil(3s, [&] { return b.sees(second); });
    a.target(second);
    const bool weakened = a.waitUntil(40s, [&] {
        const auto hits = a.hitsBy(a.selfId(), second);
        return !a.despawnsOf(second).empty() || (!hits.empty() && hits.back().targetHp <= 9);
    });
    stopWhereYouAre(a);
    if (!weakened || !a.despawnsOf(second).empty())
    {
        std::cout << "[INFO] last hit case skipped, a killed the slime\n";
        return;
    }

    const std::size_t rewardsBefore = b.rewards().size();
    b.target(second);
    check(b.waitUntil(10s, [&] { return !b.despawnsOf(second).empty(); }), "b finished the slime");
    const auto lastHits = b.hitsBy(b.selfId(), second);
    check(lastHits.size() == 1 && lastHits.front().damage < 12, "last hit damage is below contribution");
    check(waitForRewards(b, rewardsBefore + 1), "last hit is rewarded");
}

// 서버를 --debug-kill-exp 50으로 띄워야 한다. 20 + 25를 넘겨 한 번에 Lv.3이 되고 5가 남는다.
void testMultiLevel(std::uint16_t port)
{
    Client a(port);
    enterAndWait(a);
    killNearestSlime(a);
    check(waitForRewards(a, 1), "reward received");
    if (a.rewards().empty())
    {
        return;
    }

    const Reward& reward = a.rewards().front();
    const proto::PlayerStats& stats = reward.stats;
    std::cout << "[INFO] level " << stats.level() << " exp " << stats.exp() << "/" << stats.required_exp() << '\n';
    check(reward.levelsGained == 2 && stats.level() == 3, "one kill gains 2 levels");
    check(stats.exp() == 5, "excess exp carries over");
    check(stats.required_exp() == 31, "level 3 needs 31 exp");
    check(stats.max_hp() == 106 && stats.attack() == 50 && stats.defense() == 22, "stats rise per level");
    check(a.waitUntil(1s, [&] { return a.position(a.selfId()).hp == 106; }), "hp is restored to new max");
}

// 서버를 --debug-kill-exp 1000000으로 띄워야 한다.
void testLevelCap(std::uint16_t port)
{
    Client a(port);
    enterAndWait(a);
    killNearestSlime(a);
    check(waitForRewards(a, 1), "first reward received");
    if (a.rewards().empty())
    {
        return;
    }

    const proto::PlayerStats first = a.rewards().front().stats;
    std::cout << "[INFO] level " << first.level() << " exp " << first.exp() << "/" << first.required_exp()
              << " hp " << first.max_hp() << " atk " << first.attack() << " def " << first.defense() << '\n';
    check(first.level() == 20, "level stops at 20");
    check(first.exp() == 0 && first.required_exp() == 0, "max level has no exp");
    check(first.max_hp() == 157 && first.attack() == 67 && first.defense() == 39, "stats at level 20");

    const std::uint32_t second = killNearestSlime(a);
    check(waitForRewards(a, 2), "second reward received");
    if (a.rewards().size() < 2)
    {
        return;
    }
    const Reward& reward = a.rewards().back();
    check(reward.slime == second, "second reward is for second slime");
    check(reward.expGained == 0 && reward.stats.exp() == 0 && reward.stats.level() == 20, "exp ignored at max level");
    check(reward.goldGained >= 6 && reward.goldGained <= 10, "gold still given at max level");
    check(reward.stats.gold() == first.gold() + reward.goldGained, "gold accumulates at max level");
}

// 서버를 --debug-player-hp 16으로 띄워야 한다.
void testDeadContributor(std::uint16_t port, bool levelsUp)
{
    Client a(port);
    Client b(port);
    enterAndWait(a);
    enterAndWait(b);

    // shared는 나중에 b가 끝낼 슬라임, killer는 a를 죽일 슬라임이다.
    const std::uint32_t shared = waitForSlime(a);
    const std::uint32_t killer = waitForSlime(a, shared);
    b.waitUntil(3s, [&] { return b.sees(shared); });

    a.target(shared);
    check(a.waitUntil(30s, [&] { return !a.hitsBy(a.selfId(), shared).empty(); }), "a hit the shared slime");
    a.target(killer);
    check(a.waitUntil(30s, [&] { return !a.hitsBy(a.selfId(), killer).empty(); }), "a hit the second slime");
    stopWhereYouAre(a);

    // b가 때려서 추적 대상을 b로 바꿔야 a가 죽어도 shared가 복귀하지 않는다.
    b.target(shared);
    check(b.waitUntil(30s, [&] { return !b.hitsBy(b.selfId(), shared).empty(); }), "b hit the shared slime");
    stopWhereYouAre(b);

    check(a.waitUntil(40s, [&] { return !a.deaths().empty(); }), "a died");
    b.target(shared);
    check(b.waitUntil(3s, [&] { return !b.despawnsOf(shared).empty(); }), "b killed the shared slime");
    check(waitForRewards(a, 1), "dead contributor is rewarded");
    if (!a.rewards().empty())
    {
        const Death& death = a.deaths().back();
        std::cout << "[INFO] reward tick " << a.rewards().front().tick << " revive tick " << death.reviveTick << '\n';
        check(a.rewards().front().tick < death.reviveTick, "reward arrived while dead");
        if (!levelsUp)
        {
            check(a.rewards().front().expGained == 5, "dead contributor gets full exp");
            return;
        }

        // 서버를 --debug-kill-exp 20과 함께 띄운다. 사망 중 Lv.2가 된다.
        const proto::PlayerStats& stats = a.rewards().front().stats;
        check(a.rewards().front().levelsGained == 1 && stats.level() == 2 && stats.max_hp() == 19,
              "level up while dead");
        const std::uint32_t rewardTick = a.rewards().front().tick;
        check(a.waitUntil(1s, [&] { return a.lastTick() > rewardTick; }), "sync after level up");
        check(a.position(a.selfId()).state == proto::ENTITY_STATE_DEAD && a.position(a.selfId()).hp == 0,
              "hp stays 0 while dead after level up");
        check(a.position(a.selfId()).maxHp == 19 && a.position(a.selfId()).level == 2, "sync shows new max hp while dead");
        check(a.waitUntil(5s, [&] { return a.position(a.selfId()).state != proto::ENTITY_STATE_DEAD; }), "revived");
        std::cout << "[INFO] revive hp " << a.position(a.selfId()).hp << "/" << a.position(a.selfId()).maxHp << '\n';
        check(a.position(a.selfId()).hp == 19, "revive restores new max hp");
    }
}

bool waitForRejects(Client& client, std::size_t count)
{
    return client.waitUntil(2s, [&] { return client.rejectRecords().size() >= count; });
}

std::uint32_t lastSkillTick(const Client& client)
{
    return client.skills().empty() ? 0 : client.skills().back().tick;
}

bool isUnderCooldown(const Client& client, std::uint32_t endTick)
{
    return client.lastTick() < endTick;
}

// 구르기 시작 전의 IDLE을 끝으로 착각하지 않도록 ROLLING을 먼저 본다.
bool waitRollFinished(Client& client)
{
    return client.waitUntil(2s, [&] { return client.position(client.selfId()).state == proto::ENTITY_STATE_ROLLING; })
        && client.waitUntil(2s, [&] { return client.position(client.selfId()).state == proto::ENTITY_STATE_IDLE; });
}

void testSkillRules(std::uint16_t port)
{
    Client a(port);
    enterAndWait(a);
    a.waitUntil(2s, [&] { return a.sees(a.selfId()); });

    a.skill(proto::SKILL_ID_ROLL, 0.0f, 0.0f);
    a.skill(proto::SKILL_ID_ROLL, 2.0f, 0.0f);
    a.skill(proto::SKILL_ID_ROLL, std::nanf(""), 0.0f);
    a.skill(static_cast<proto::SkillId>(99));
    check(waitForRejects(a, 4), "invalid skill requests rejected");
    if (a.rejectRecords().size() >= 4)
    {
        const auto& rejects = a.rejectRecords();
        check(rejects[0].reason == proto::REJECT_REASON_INVALID_DIRECTION
                  && rejects[1].reason == proto::REJECT_REASON_INVALID_DIRECTION
                  && rejects[2].reason == proto::REJECT_REASON_INVALID_DIRECTION, "bad direction rejected");
        check(rejects[3].reason == proto::REJECT_REASON_INVALID_SKILL, "unknown skill rejected");
    }
    check(a.skills().empty(), "rejected skills do not start");

    const std::size_t rejectsBefore = a.rejectRecords().size();
    a.skill(proto::SKILL_ID_ROLL, 1.0f, 0.0f);
    a.skill(proto::SKILL_ID_SPIN_KICK);
    check(waitForRejects(a, rejectsBefore + 1)
              && a.rejectRecords().back().reason == proto::REJECT_REASON_SKILL_WHILE_ROLLING,
          "skill while rolling rejected");
    check(a.waitUntil(2s, [&] { return !a.skills().empty(); }), "roll started");
    if (a.skills().empty())
    {
        return;
    }
    const SkillRecord roll = a.skills().back();
    check(roll.cooldownEndTick - roll.tick == 30, "roll cooldown is 3 seconds");

    check(waitRollFinished(a), "roll finished");
    const SeenPosition& self = a.position(a.selfId());
    std::cout << "[INFO] rolled to " << self.x << ", " << self.y << '\n';
    check(self.x == 890.0f && self.y == 800.0f, "roll moves 90 in a straight line");

    int rollingTicks = 0;
    for (const SeenPosition& seen : a.history(a.selfId()))
    {
        rollingTicks += seen.state == proto::ENTITY_STATE_ROLLING ? 1 : 0;
    }
    check(rollingTicks == 3, "rolling lasts 0.3 seconds");

    const std::size_t rejectsAfterRoll = a.rejectRecords().size();
    a.skill(proto::SKILL_ID_ROLL, -1.0f, 0.0f);
    check(waitForRejects(a, rejectsAfterRoll + 1)
              && a.rejectRecords().back().reason == proto::REJECT_REASON_SKILL_COOLDOWN, "roll cooldown rejected");
    if (!a.rejectRecords().empty())
    {
        const RejectRecord& reject = a.rejectRecords().back();
        std::cout << "[INFO] roll remaining ms " << reject.remainingMs << '\n';
        check(reject.skill == proto::SKILL_ID_ROLL && reject.remainingMs > 0 && reject.remainingMs <= 3000,
              "reject reports remaining cooldown");
    }

    // 공용 쿨다운은 없으므로 구르기 쿨다운 중에도 회전차기를 쓸 수 있다.
    const std::size_t skillsBefore = a.skills().size();
    a.skill(proto::SKILL_ID_SPIN_KICK);
    check(a.waitUntil(2s, [&] { return a.skills().size() > skillsBefore; }), "no shared cooldown");
    if (a.skills().size() > skillsBefore)
    {
        check(a.skills().back().cooldownEndTick - a.skills().back().tick == 55, "spin cooldown is 5.5 seconds");
    }

    const std::size_t rejectsBeforeSpin = a.rejectRecords().size();
    a.skill(proto::SKILL_ID_SPIN_KICK);
    check(waitForRejects(a, rejectsBeforeSpin + 1)
              && a.rejectRecords().back().reason == proto::REJECT_REASON_SKILL_COOLDOWN, "spin cooldown rejected");

    a.waitUntil(4s, [&] { return !isUnderCooldown(a, roll.cooldownEndTick + 2); });
    const std::size_t skillsAfterWait = a.skills().size();
    a.skill(proto::SKILL_ID_ROLL, -1.0f, 0.0f);
    check(a.waitUntil(2s, [&] { return a.skills().size() > skillsAfterWait; }), "roll usable after cooldown");
}

void testRollHit(std::uint16_t port)
{
    Client a(port);
    enterAndWait(a);
    const std::uint32_t slime = waitForSlime(a);
    const SeenPosition first = a.position(slime);
    const SeenPosition start = a.position(a.selfId());
    const float gap = distanceBetween(start.x, start.y, first.x, first.y);
    const float ratio = 45.0f / gap;
    moveAndArrive(a, first.x + (start.x - first.x) * ratio, first.y + (start.y - first.y) * ratio);

    const SeenPosition self = a.position(a.selfId());
    const SeenPosition seen = a.position(slime);
    const float distance = distanceBetween(self.x, self.y, seen.x, seen.y);
    a.skill(proto::SKILL_ID_ROLL, (seen.x - self.x) / distance, (seen.y - self.y) / distance);
    check(a.waitUntil(3s, [&] { return !a.hitsBy(a.selfId(), slime).empty(); }), "roll hits slime on its path");
    a.pump(500ms);

    const auto hits = a.hitsBy(a.selfId(), slime);
    if (hits.empty())
    {
        return;
    }
    std::cout << "[INFO] roll damage " << hits.front().damage << '\n';
    check(hits.size() == 1, "roll hits each target once");
    check(hits.front().skill == proto::SKILL_ID_ROLL, "combat result names roll");
    check(hits.front().damage >= 90 && hits.front().damage <= 93, "roll deals double damage");

    check(a.waitUntil(8s, [&] { return !a.hitsBy(slime, a.selfId()).empty(); }), "slime chases after skill hit");

    a.skill(proto::SKILL_ID_SPIN_KICK);
    check(a.waitUntil(3s, [&] { return !a.despawnsOf(slime).empty(); }), "spin kick kills weakened slime");
    const auto spinHits = a.hitsBy(a.selfId(), slime);
    if (spinHits.size() == 2)
    {
        std::cout << "[INFO] spin damage " << spinHits.back().damage << '\n';
        check(spinHits.back().skill == proto::SKILL_ID_SPIN_KICK, "combat result names spin kick");
        check(spinHits.back().damage >= 72 && spinHits.back().damage <= 75, "spin deals 1.6x damage");
    }
    check(waitForRewards(a, 1), "skill kill gives reward");
    if (!a.rewards().empty())
    {
        check(a.rewards().front().expGained == 5 && a.rewards().front().slime == slime, "reward is for skill kill");
    }
}

// 첫 타격까지 진행한다. 앞 시나리오의 슬라임이 복귀 중이면 거부되므로 다른 슬라임을 고른다.
std::uint32_t engageSlime(Client& client)
{
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        const std::uint32_t slime = waitForSlime(client);
        const std::size_t rejects = client.rejects().size();
        client.target(slime);
        client.waitUntil(30s, [&] { return !client.hitsBy(client.selfId(), slime).empty() || client.rejects().size() > rejects; });
        if (!client.hitsBy(client.selfId(), slime).empty())
        {
            return slime;
        }
        client.pump(1s);
    }
    return 0;
}

void testRollInputs(std::uint16_t port)
{
    Client a(port);
    enterAndWait(a);

    // 구르기 전에 지정한 대상은 끝난 뒤 이어서 공격한다.
    const std::uint32_t attackTarget = engageSlime(a);
    check(attackTarget != 0, "engaged a slime");
    a.skill(proto::SKILL_ID_ROLL, 1.0f, 0.0f);
    check(a.waitUntil(4s, [&] { return a.hitsBy(a.selfId(), attackTarget).size() >= 2; }),
          "attack resumes after roll");
    stopWhereYouAre(a);

    // 구르기 중 대상 지정은 끝난 뒤 적용한다.
    a.waitUntil(5s, [&] { return !isUnderCooldown(a, a.skills().back().cooldownEndTick + 1); });
    const std::uint32_t pending = waitForSlime(a, attackTarget);
    a.skill(proto::SKILL_ID_ROLL, -1.0f, 0.0f);
    a.target(pending);
    check(a.waitUntil(20s, [&] { return !a.hitsBy(a.selfId(), pending).empty(); }), "target during roll applies after");
    stopWhereYouAre(a);

    // 구르기 중 입력은 마지막 것만 남는다. 대상 뒤의 이동이 이긴다.
    a.waitUntil(5s, [&] { return !isUnderCooldown(a, a.skills().back().cooldownEndTick + 1); });
    const std::uint32_t skipped = waitForSlime(a, pending);
    const auto basicHits = [&] {
        const auto hits = a.hitsBy(a.selfId(), skipped);
        return std::count_if(hits.begin(), hits.end(),
                             [](const Hit& hit) { return hit.skill == proto::SKILL_ID_UNSPECIFIED; });
    };
    const auto hitsBefore = basicHits();
    const SeenPosition self = a.position(a.selfId());
    const float destinationX = self.x < 800.0f ? self.x + 120.0f : self.x - 120.0f;
    a.skill(proto::SKILL_ID_ROLL, 0.0f, 1.0f);
    a.target(skipped);
    a.moveTo(destinationX, self.y);
    check(a.waitUntil(5s, [&] {
              return a.position(a.selfId()).x == destinationX && a.position(a.selfId()).y == self.y;
          }), "move during roll applies after");
    // 구르기 경로의 스킬 피해는 대상 지정과 무관하므로 기본 공격만 센다.
    check(basicHits() == hitsBefore, "last input wins over target");
}

void testRollBoundary(std::uint16_t port)
{
    Client a(port);
    enterAndWait(a);
    check(moveAndArrive(a, 1590.0f, 800.0f), "moved near the edge");

    a.skill(proto::SKILL_ID_ROLL, 1.0f, 0.0f);
    check(waitRollFinished(a), "roll ended");
    check(a.position(a.selfId()).x == 1600.0f && a.position(a.selfId()).y == 800.0f, "roll stops at map edge");

    int rollingTicks = 0;
    for (const SeenPosition& seen : a.history(a.selfId()))
    {
        rollingTicks += seen.state == proto::ENTITY_STATE_ROLLING ? 1 : 0;
    }
    check(rollingTicks == 3, "rolling lasts full time at the edge");

    a.waitUntil(5s, [&] { return !isUnderCooldown(a, a.skills().back().cooldownEndTick + 1); });
    check(moveAndArrive(a, 1590.0f, 800.0f), "moved back from the edge");
    a.skill(proto::SKILL_ID_ROLL, 0.6f, 0.8f);
    a.pump(1s);
    const SeenPosition& self = a.position(a.selfId());
    std::cout << "[INFO] diagonal stop " << self.x << ", " << self.y << '\n';
    check(self.x == 1600.0f && std::abs(self.y - 813.33f) < 0.1f, "diagonal roll does not slide along the edge");
}

// 서버를 --debug-slime-cluster 1로 띄워야 한다. 입장 위치에 슬라임이 모여 있다.
void testSpinMaxTargets(std::uint16_t port)
{
    Client a(port);
    enterAndWait(a);
    a.waitUntil(3s, [&] { return a.sees(a.selfId()) && a.nearestSlime() != 0; });
    a.pump(300ms);

    a.skill(proto::SKILL_ID_SPIN_KICK);
    a.waitUntil(2s, [&] { return a.allHits().size() >= 10; });
    a.pump(500ms);

    std::vector<std::uint32_t> hitSlimes;
    for (const Hit& hit : a.allHits())
    {
        if (hit.attacker == a.selfId() && hit.skill == proto::SKILL_ID_SPIN_KICK)
        {
            hitSlimes.push_back(hit.target);
        }
    }
    std::cout << "[INFO] spin targets " << hitSlimes.size() << '\n';
    check(hitSlimes.size() == 10, "spin kick hits at most 10 targets");

    // 위치를 처음 본 스냅샷 기준으로 맞은 슬라임이 안 맞은 슬라임보다 가까워야 한다.
    const SeenPosition self = a.history(a.selfId()).front();
    float farthestHit = 0.0f;
    float nearestMissed = 1e9f;
    for (std::uint32_t id = 1; id <= 30; ++id)
    {
        if (a.history(id).empty())
        {
            continue;
        }
        const SeenPosition& seen = a.history(id).back();
        const float distance = distanceBetween(self.x, self.y, seen.x, seen.y);
        if (std::find(hitSlimes.begin(), hitSlimes.end(), id) != hitSlimes.end())
        {
            farthestHit = std::max(farthestHit, distance);
        }
        else
        {
            nearestMissed = std::min(nearestMissed, distance);
        }
    }
    std::cout << "[INFO] farthest hit " << farthestHit << ", nearest missed " << nearestMissed << '\n';
    check(farthestHit <= nearestMissed + 10.0f, "spin kick picks nearest targets");
}

// 서버를 --debug-slime-cluster 1 --debug-player-attack 10으로 띄워야 한다. 슬라임이 죽지 않는다.
void testStun(std::uint16_t port)
{
    Client a(port);
    Client b(port);
    enterAndWait(a);
    enterAndWait(b);
    a.waitUntil(3s, [&] { return a.nearestSlime() != 0; });
    b.waitUntil(3s, [&] { return b.nearestSlime() != 0; });

    a.skill(proto::SKILL_ID_SPIN_KICK);
    a.pump(500ms);
    b.skill(proto::SKILL_ID_SPIN_KICK);
    b.waitUntil(2s, [&] { return !b.skills().empty(); });
    a.pump(2500ms);
    b.pump(100ms);

    std::uint32_t slime = 0;
    for (const Hit& hit : a.allHits())
    {
        if (hit.attacker == a.selfId() && !b.hitsBy(b.selfId(), hit.target).empty())
        {
            slime = hit.target;
            break;
        }
    }
    check(slime != 0, "same slime stunned twice");
    if (slime == 0)
    {
        return;
    }

    std::uint32_t firstStunned = 0;
    std::uint32_t lastStunned = 0;
    bool movedWhileStunned = false;
    const auto& history = a.history(slime);
    for (std::size_t i = 1; i < history.size(); ++i)
    {
        if (history[i].state != proto::ENTITY_STATE_STUNNED)
        {
            continue;
        }
        firstStunned = firstStunned == 0 ? history[i].tick : firstStunned;
        lastStunned = history[i].tick;
        movedWhileStunned = movedWhileStunned
                         || (history[i - 1].state == proto::ENTITY_STATE_STUNNED
                             && (history[i].x != history[i - 1].x || history[i].y != history[i - 1].y));
    }
    const std::uint32_t stunTicks = lastStunned - firstStunned + 1;
    // a는 AOI 안 b의 스킬도 받으므로 처음과 마지막 기록이 각자의 스킬이다.
    const std::uint32_t gap = a.skills().back().tick - a.skills().front().tick;
    std::cout << "[INFO] stun ticks " << stunTicks << ", second stun after " << gap << '\n';
    check(stunTicks == gap + 10, "second stun overwrites remaining time");
    check(!movedWhileStunned, "slime does not move while stunned");

    bool attackedWhileStunned = false;
    for (const Hit& hit : a.allHits())
    {
        attackedWhileStunned = attackedWhileStunned
                            || (hit.attacker == slime && hit.tick >= firstStunned && hit.tick < lastStunned);
    }
    check(!attackedWhileStunned, "slime does not attack while stunned");

    check(a.waitUntil(6s, [&] {
              for (const Hit& hit : a.allHits())
              {
                  if (hit.attacker == slime)
                  {
                      return true;
                  }
              }
              for (const Hit& hit : b.allHits())
              {
                  if (hit.attacker == slime)
                  {
                      return true;
                  }
              }
              b.pump(10ms);
              return false;
          }), "slime resumes attacking after stun");
}

// 서버를 --debug-player-hp 5 --debug-player-attack 1로 띄워야 한다.
// HP 5는 슬라임의 두 번째 타격에 죽는다. 그 틱에 구르기가 겹치도록 맞춘다.
void testRollDeath(std::uint16_t port)
{
    Client a(port);
    enterAndWait(a);

    for (int attempt = 1; attempt <= 9; ++attempt)
    {
        const std::uint32_t slime = waitForSlime(a);
        const std::size_t deathsBefore = a.deaths().size();
        const std::size_t hitsBefore = a.hitsBy(slime, a.selfId()).size();
        a.target(slime);
        a.waitUntil(30s, [&] { return a.hitsBy(slime, a.selfId()).size() > hitsBefore; });
        const auto counter = a.hitsBy(slime, a.selfId());
        if (counter.size() <= hitsBefore)
        {
            continue;
        }

        const std::uint32_t firstHit = counter.back().tick;
        // 지연에 따라 도착 틱이 달라지므로 시도마다 보내는 시점을 바꾼다.
        const std::uint32_t lead = 18 + static_cast<std::uint32_t>(attempt % 3);
        a.waitUntil(3s, [&] { return a.lastTick() >= firstHit + lead; });
        const SeenPosition self = a.position(a.selfId());
        const SeenPosition seen = a.position(slime);
        const float distance = std::max(distanceBetween(self.x, self.y, seen.x, seen.y), 1.0f);
        a.skill(proto::SKILL_ID_ROLL, (seen.x - self.x) / distance, (seen.y - self.y) / distance);

        a.waitUntil(3s, [&] { return a.deaths().size() > deathsBefore; });
        if (a.deaths().size() <= deathsBefore || a.skills().empty())
        {
            a.waitUntil(5s, [&] { return a.position(a.selfId()).state != proto::ENTITY_STATE_DEAD; });
            continue;
        }

        const Death death = a.deaths().back();
        const SkillRecord roll = a.skills().back();
        const std::uint32_t since = death.tick - roll.tick;
        std::cout << "[INFO] attempt " << attempt << " died " << since << " ticks after roll\n";
        if (since > 2)
        {
            a.waitUntil(5s, [&] { return a.position(a.selfId()).state != proto::ENTITY_STATE_DEAD; });
            a.pump(3200ms);
            continue;
        }

        a.pump(300ms);
        const SeenPosition dead = a.position(a.selfId());
        const float rolled = distanceBetween(self.x, self.y, dead.x, dead.y);
        std::cout << "[INFO] rolled " << rolled << " before stopping\n";
        if (rolled >= 89.0f)
        {
            // 마지막 구르기 틱에 죽으면 이미 90을 다 움직인 뒤다. 다시 시도한다.
            a.waitUntil(5s, [&] { return a.position(a.selfId()).state != proto::ENTITY_STATE_DEAD; });
            a.pump(3200ms);
            continue;
        }
        check(rolled < 89.0f, "roll stops at death");
        a.pump(500ms);
        check(a.position(a.selfId()).x == dead.x && a.position(a.selfId()).y == dead.y, "dead player stays still");
        check(a.skills().back().cooldownEndTick == roll.cooldownEndTick, "cooldown is kept after death");

        const std::size_t rejectsBefore = a.rejectRecords().size();
        a.skill(proto::SKILL_ID_SPIN_KICK);
        a.pump(300ms);
        check(a.rejectRecords().size() == rejectsBefore, "skill ignored while dead");
        check(a.waitUntil(5s, [&] { return a.position(a.selfId()).state != proto::ENTITY_STATE_DEAD; }), "revived");
        check(a.position(a.selfId()).hp == 5, "hp full after revive");
        return;
    }
    check(false, "could not die during roll");
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
        for (const auto& client : clients)
        {
            client->drain();
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

        if (argc > 2)
        {
            const std::string mode = argv[2];
            if (mode == "death")
            {
                testPlayerDeath(port);
            }
            else if (mode == "lowattack")
            {
                testContributionThreshold(port);
            }
            else if (mode == "multilevel")
            {
                testMultiLevel(port);
            }
            else if (mode == "levelcap")
            {
                testLevelCap(port);
            }
            else if (mode == "deadreward")
            {
                testDeadContributor(port, false);
            }
            else if (mode == "deadlevel")
            {
                testDeadContributor(port, true);
            }
            else if (mode == "cluster")
            {
                testSpinMaxTargets(port);
            }
            else if (mode == "stun")
            {
                testStun(port);
            }
            else if (mode == "rolldeath")
            {
                testRollDeath(port);
            }
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
        testLevelUp(port);
        testSharedKill(port);
        testContributionReset(port);
        testSkillRules(port);
        testRollHit(port);
        testRollInputs(port);
        testRollBoundary(port);
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
