#include "LagTransport.h"

#include <algorithm>

namespace trymmo
{
using std::chrono::ceil;
using std::chrono::milliseconds;

LagTransport::LagTransport(ITransport& inner, milliseconds oneWayDelay)
    : inner_(inner), oneWayDelay_(oneWayDelay)
{
    inner_.setHandler(this);
}

void LagTransport::poll(milliseconds timeout)
{
    const auto now = Clock::now();
    auto wait = timeout;
    for (const auto* queue : {&inbound_, &outbound_})
    {
        if (!queue->empty())
        {
            wait = std::min(wait, std::max(milliseconds(0), ceil<milliseconds>(queue->front().due - now)));
        }
    }

    inner_.poll(wait);
    releaseDue();
}

void LagTransport::send(ConnectionId connection, std::span<const std::byte> frame)
{
    outbound_.push_back({Clock::now() + oneWayDelay_, EventType::Frame, connection, {frame.begin(), frame.end()}});
}

void LagTransport::close(ConnectionId connection)
{
    outbound_.push_back({Clock::now() + oneWayDelay_, EventType::Close, connection, {}});
}

void LagTransport::onConnected(ConnectionId connection)
{
    enqueueInbound(EventType::Connected, connection, {});
}

void LagTransport::onDisconnected(ConnectionId connection)
{
    enqueueInbound(EventType::Disconnected, connection, {});
}

void LagTransport::onFrame(ConnectionId connection, std::span<const std::byte> frame)
{
    enqueueInbound(EventType::Frame, connection, frame);
}

void LagTransport::enqueueInbound(EventType type, ConnectionId connection, std::span<const std::byte> frame)
{
    inbound_.push_back({Clock::now() + oneWayDelay_, type, connection, {frame.begin(), frame.end()}});
}

void LagTransport::releaseDue()
{
    const auto now = Clock::now();

    while (!outbound_.empty() && outbound_.front().due <= now)
    {
        const Event event = std::move(outbound_.front());
        outbound_.pop_front();
        if (event.type == EventType::Close)
        {
            inner_.close(event.connection);
        }
        else
        {
            inner_.send(event.connection, event.frame);
        }
    }

    while (!inbound_.empty() && inbound_.front().due <= now)
    {
        const Event event = std::move(inbound_.front());
        inbound_.pop_front();
        switch (event.type)
        {
        case EventType::Connected:
            handler_->onConnected(event.connection);
            break;
        case EventType::Disconnected:
            handler_->onDisconnected(event.connection);
            break;
        case EventType::Frame:
            handler_->onFrame(event.connection, event.frame);
            break;
        case EventType::Close:
            break;
        }
    }
}
}
