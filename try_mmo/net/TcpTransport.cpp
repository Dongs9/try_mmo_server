#include "TcpTransport.h"

#include "../Log.h"

#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

#include <ws2tcpip.h>

namespace trymmo
{
namespace
{
constexpr std::size_t kLengthPrefixSize = 2;
constexpr std::size_t kMinFrameSize = 2;
constexpr std::size_t kMaxInboundFrameSize = 8192;   // 입력 메시지는 작다. 서버 보호용 제한.
constexpr std::size_t kMaxOutboundFrameSize = 65535;  // 2바이트 길이 필드의 한도.
// 느린 클라이언트 때문에 메모리가 끝없이 늘지 않도록 막는다.
constexpr std::size_t kMaxOutboxSize = 256 * 1024;

void setNonBlocking(SOCKET socket)
{
    u_long enabled = 1;
    if (ioctlsocket(socket, FIONBIO, &enabled) == SOCKET_ERROR)
    {
        throw std::runtime_error("ioctlsocket failed: " + std::to_string(WSAGetLastError()));
    }
}
}

WinsockRuntime::WinsockRuntime()
{
    WSADATA wsaData{};
    if (const int error = WSAStartup(MAKEWORD(2, 2), &wsaData); error != 0)
    {
        throw std::runtime_error("WSAStartup failed: " + std::to_string(error));
    }
}

WinsockRuntime::~WinsockRuntime()
{
    WSACleanup();
}

Socket::~Socket()
{
    if (handle_ != INVALID_SOCKET)
    {
        closesocket(handle_);
    }
}

TcpTransport::TcpTransport(std::uint16_t port)
    : listener_(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP))
{
    if (listener_.get() == INVALID_SOCKET)
    {
        throw std::runtime_error("socket failed: " + std::to_string(WSAGetLastError()));
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(listener_.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
    {
        throw std::runtime_error("bind failed: " + std::to_string(WSAGetLastError()));
    }
    if (listen(listener_.get(), SOMAXCONN) == SOCKET_ERROR)
    {
        throw std::runtime_error("listen failed: " + std::to_string(WSAGetLastError()));
    }
    setNonBlocking(listener_.get());
}

void TcpTransport::poll(std::chrono::milliseconds timeout)
{
    // 0번은 항상 리슨 소켓이고, 나머지는 ids와 순서가 같다.
    std::vector<WSAPOLLFD> descriptors;
    std::vector<ConnectionId> ids;
    descriptors.reserve(connections_.size() + 1);
    ids.reserve(connections_.size());

    descriptors.push_back({listener_.get(), POLLRDNORM, 0});
    for (const auto& [id, connection] : connections_)
    {
        SHORT events = POLLRDNORM;
        if (!connection->outbox.empty())
        {
            events |= POLLWRNORM;
        }
        descriptors.push_back({connection->socket.get(), events, 0});
        ids.push_back(id);
    }

    const int ready = WSAPoll(descriptors.data(), static_cast<ULONG>(descriptors.size()),
                              static_cast<INT>(timeout.count()));
    if (ready == SOCKET_ERROR)
    {
        throw std::runtime_error("WSAPoll failed: " + std::to_string(WSAGetLastError()));
    }

    if (descriptors[0].revents & POLLRDNORM)
    {
        acceptAll();
    }

    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        const SHORT revents = descriptors[i + 1].revents;
        Connection& connection = *connections_.at(ids[i]);
        if (revents & (POLLERR | POLLNVAL))
        {
            connection.closing = true;
            continue;
        }
        // 상대가 끊으면 POLLHUP만 오기도 한다. recv가 0을 돌려주는 경로로 합친다.
        if (revents & (POLLRDNORM | POLLHUP))
        {
            receive(ids[i], connection);
        }
        if ((revents & POLLWRNORM) && !connection.closing)
        {
            flush(connection);
        }
    }

    removeClosed();
}

void TcpTransport::send(ConnectionId connection, std::span<const std::byte> frame)
{
    const auto found = connections_.find(connection);
    if (found == connections_.end() || found->second->closing)
    {
        return;
    }

    if (frame.size() < kMinFrameSize || frame.size() > kMaxOutboundFrameSize)
    {
        throw std::logic_error("invalid outgoing frame size: " + std::to_string(frame.size()));
    }

    Connection& target = *found->second;
    if (target.outbox.size() + kLengthPrefixSize + frame.size() > kMaxOutboxSize)
    {
        logWarn("connection " + std::to_string(connection) + " outbox overflow, closing");
        target.closing = true;
        return;
    }

    const auto length = static_cast<std::uint16_t>(frame.size());
    target.outbox.push_back(static_cast<std::byte>(length & 0xFF));
    target.outbox.push_back(static_cast<std::byte>(length >> 8));
    target.outbox.insert(target.outbox.end(), frame.begin(), frame.end());
}

void TcpTransport::close(ConnectionId connection)
{
    if (const auto found = connections_.find(connection); found != connections_.end())
    {
        found->second->closing = true;
    }
}

void TcpTransport::acceptAll()
{
    for (;;)
    {
        const SOCKET accepted = accept(listener_.get(), nullptr, nullptr);
        if (accepted == INVALID_SOCKET)
        {
            const int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK)
            {
                logWarn("accept failed: " + std::to_string(error));
            }
            return;
        }

        auto connection = std::make_unique<Connection>(accepted);
        setNonBlocking(accepted);
        // 틱마다 보내는 작은 패킷이 Nagle 알고리즘에 묶여 늦어지지 않게 한다.
        const BOOL noDelay = TRUE;
        setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

        const ConnectionId id = nextConnectionId_++;
        connections_.emplace(id, std::move(connection));
        handler_->onConnected(id);
    }
}

void TcpTransport::receive(ConnectionId id, Connection& connection)
{
    std::array<char, 4096> buffer;
    for (;;)
    {
        const int received = recv(connection.socket.get(), buffer.data(), static_cast<int>(buffer.size()), 0);
        if (received > 0)
        {
            const auto* bytes = reinterpret_cast<const std::byte*>(buffer.data());
            connection.inbox.insert(connection.inbox.end(), bytes, bytes + received);
            continue;
        }

        if (received == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
        {
            break;
        }

        // 0은 상대가 정상 종료한 것이고, 그 외 오류는 연결이 깨진 것이다.
        connection.closing = true;
        break;
    }

    extractFrames(id, connection);
}

void TcpTransport::flush(Connection& connection)
{
    std::size_t sent = 0;
    while (sent < connection.outbox.size())
    {
        const auto* bytes = reinterpret_cast<const char*>(connection.outbox.data()) + sent;
        const int result = ::send(connection.socket.get(), bytes,
                                  static_cast<int>(connection.outbox.size() - sent), 0);
        if (result == SOCKET_ERROR)
        {
            if (WSAGetLastError() != WSAEWOULDBLOCK)
            {
                connection.closing = true;
            }
            break;
        }
        sent += static_cast<std::size_t>(result);
    }
    connection.outbox.erase(connection.outbox.begin(), connection.outbox.begin() + sent);
}

void TcpTransport::extractFrames(ConnectionId id, Connection& connection)
{
    std::size_t offset = 0;
    while (connection.inbox.size() - offset >= kLengthPrefixSize)
    {
        const std::size_t length = std::to_integer<std::size_t>(connection.inbox[offset])
                                 | (std::to_integer<std::size_t>(connection.inbox[offset + 1]) << 8);
        if (length < kMinFrameSize || length > kMaxInboundFrameSize)
        {
            logWarn("connection " + std::to_string(id) + " sent invalid frame length " + std::to_string(length));
            connection.closing = true;
            break;
        }
        if (connection.inbox.size() - offset - kLengthPrefixSize < length)
        {
            break;
        }

        handler_->onFrame(id, std::span(connection.inbox).subspan(offset + kLengthPrefixSize, length));
        offset += kLengthPrefixSize + length;
    }
    connection.inbox.erase(connection.inbox.begin(), connection.inbox.begin() + offset);
}

void TcpTransport::removeClosed()
{
    for (auto it = connections_.begin(); it != connections_.end();)
    {
        if (!it->second->closing)
        {
            ++it;
            continue;
        }

        const ConnectionId id = it->first;
        it = connections_.erase(it);
        handler_->onDisconnected(id);
    }
}
}
