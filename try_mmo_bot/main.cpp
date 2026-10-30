// 실제 메시지로 서버를 시험하는 봇. P1은 접속, 이동, 동기화, AOI 규칙을 시나리오로 확인한다.
#include "Packet.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
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
};

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
            break;
        }
        case proto::S2C_ENTITY_DESPAWN:
        {
            proto::EntityDespawn message;
            trymmo::decodePayload(payload, message);
            spawned_.erase(message.entity_id());
            positions_.erase(message.entity_id());
            despawns_.push_back({message.entity_id(), message.reason()});
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
    check(!b.despawns().empty() && b.despawns().back().reason == proto::DESPAWN_REASON_OUT_OF_AOI, "reason is out of aoi");
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
    check(a.waitUntil(2s, [&] { return !a.despawns().empty(); }), "disconnect despawns");
    check(!a.despawns().empty() && a.despawns().back().entity == otherId
              && a.despawns().back().reason == proto::DESPAWN_REASON_LEFT,
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

void testZoneFull(std::uint16_t port)
{
    std::vector<std::unique_ptr<Client>> clients;
    bool allEntered = true;
    for (int i = 0; i < 100; ++i)
    {
        clients.push_back(std::make_unique<Client>(port));
        allEntered = enterAndWait(*clients.back()) && clients.back()->enterResult() == proto::ENTER_RESULT_OK && allEntered;
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

        testEnterAndSpawn(port);
        testMove(port);
        testTickRate(port);
        testAoi(port);
        testDisconnect(port);
        testBadInput(port);
        testRequestLimit(port);
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
