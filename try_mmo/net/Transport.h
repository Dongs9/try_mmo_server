#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace trymmo
{
using ConnectionId = std::uint32_t;

// 전송 계층이 게임 쪽으로 올려 보내는 사건. 모두 poll()을 호출한 스레드에서 불린다.
class ITransportHandler
{
public:
    virtual ~ITransportHandler() = default;

    virtual void onConnected(ConnectionId connection) = 0;
    virtual void onDisconnected(ConnectionId connection) = 0;
    // frame은 메시지 하나 분량이며 호출이 끝나면 무효가 된다.
    virtual void onFrame(ConnectionId connection, std::span<const std::byte> frame) = 0;
};

// 게임 로직이 아는 전송 계층의 전부. TCP인지 WebSocket인지는 이 뒤에 숨는다.
class ITransport
{
public:
    virtual ~ITransport() = default;

    virtual void setHandler(ITransportHandler* handler) = 0;
    // 입출력을 처리한다. 할 일이 없으면 timeout까지 기다린다.
    virtual void poll(std::chrono::milliseconds timeout) = 0;
    virtual void send(ConnectionId connection, std::span<const std::byte> frame) = 0;
    virtual void close(ConnectionId connection) = 0;
};
}
