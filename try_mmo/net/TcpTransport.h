#pragma once

#include "Transport.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <winsock2.h>

namespace trymmo
{
// Winsock 라이브러리의 수명을 묶는다. 소켓보다 먼저 만들고 나중에 없앤다.
class WinsockRuntime
{
public:
    WinsockRuntime();
    ~WinsockRuntime();

    WinsockRuntime(const WinsockRuntime&) = delete;
    WinsockRuntime& operator=(const WinsockRuntime&) = delete;
};

// Winsock 소켓을 소유한다. 소멸할 때 닫는다.
class Socket
{
public:
    explicit Socket(SOCKET handle) : handle_(handle) {}
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    SOCKET get() const { return handle_; }

private:
    SOCKET handle_;
};

// 논블로킹 소켓과 WSAPoll로 만든 단일 스레드 TCP 서버.
// 스트림을 2바이트 길이 프리픽스로 잘라 프레임 단위로 올린다.
class TcpTransport final : public ITransport
{
public:
    explicit TcpTransport(std::uint16_t port);

    void setHandler(ITransportHandler* handler) override { handler_ = handler; }
    void poll(std::chrono::milliseconds timeout) override;
    void send(ConnectionId connection, std::span<const std::byte> frame) override;
    void close(ConnectionId connection) override;

private:
    struct Connection
    {
        explicit Connection(SOCKET handle) : socket(handle) {}

        Socket socket;
        std::vector<std::byte> inbox;   // 아직 프레임이 되지 못한 수신 바이트
        std::vector<std::byte> outbox;  // 아직 커널에 넘기지 못한 송신 바이트
        bool closing = false;
    };

    void acceptAll();
    void receive(ConnectionId id, Connection& connection);
    void flush(Connection& connection);
    void extractFrames(ConnectionId id, Connection& connection);
    void removeClosed();

    WinsockRuntime winsock_;
    ITransportHandler* handler_ = nullptr;
    Socket listener_;
    ConnectionId nextConnectionId_ = 1;
    std::unordered_map<ConnectionId, std::unique_ptr<Connection>> connections_;
};
}
