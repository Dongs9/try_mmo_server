#pragma once

#include "Transport.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <vector>

namespace trymmo
{
// 디버그 옵션. 다른 전송 계층을 감싸 송수신을 각각 oneWayDelay만큼 늦춘다.
// 게임 로직은 이 클래스의 존재를 모른다.
class LagTransport final : public ITransport, private ITransportHandler
{
public:
    LagTransport(ITransport& inner, std::chrono::milliseconds oneWayDelay);

    void setHandler(ITransportHandler* handler) override { handler_ = handler; }
    void poll(std::chrono::milliseconds timeout) override;
    void send(ConnectionId connection, std::span<const std::byte> frame) override;
    void close(ConnectionId connection) override;

private:
    using Clock = std::chrono::steady_clock;

    enum class EventType : std::uint8_t
    {
        Connected,
        Disconnected,
        Frame,
        Close
    };

    struct Event
    {
        Clock::time_point due;
        EventType type;
        ConnectionId connection;
        std::vector<std::byte> frame;
    };

    void onConnected(ConnectionId connection) override;
    void onDisconnected(ConnectionId connection) override;
    void onFrame(ConnectionId connection, std::span<const std::byte> frame) override;

    void enqueueInbound(EventType type, ConnectionId connection, std::span<const std::byte> frame);
    void releaseDue();

    ITransport& inner_;
    ITransportHandler* handler_ = nullptr;
    std::chrono::milliseconds oneWayDelay_;
    // 지연이 일정하므로 두 큐 모두 시각 순서로 쌓인다.
    std::deque<Event> inbound_;
    std::deque<Event> outbound_;
};
}
