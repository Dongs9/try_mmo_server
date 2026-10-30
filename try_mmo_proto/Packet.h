#pragma once

#include "packet.pb.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace trymmo
{
constexpr std::size_t kMsgIdSize = 2;

// 프레임: 메시지 ID 2바이트(리틀 엔디언) + Protobuf 본문. 길이 프리픽스는 전송 계층 몫이다.
inline proto::MsgId readMsgId(std::span<const std::byte> frame)
{
    const int value = std::to_integer<int>(frame[0]) | (std::to_integer<int>(frame[1]) << 8);
    return static_cast<proto::MsgId>(value);
}

inline std::vector<std::byte> encodeFrame(proto::MsgId msgId, const google::protobuf::MessageLite& message)
{
    const std::size_t payloadSize = message.ByteSizeLong();
    std::vector<std::byte> frame(kMsgIdSize + payloadSize);
    frame[0] = static_cast<std::byte>(msgId & 0xFF);
    frame[1] = static_cast<std::byte>((msgId >> 8) & 0xFF);
    message.SerializeToArray(frame.data() + kMsgIdSize, static_cast<int>(payloadSize));
    return frame;
}

inline bool decodePayload(std::span<const std::byte> payload, google::protobuf::MessageLite& message)
{
    return message.ParseFromArray(payload.data(), static_cast<int>(payload.size()));
}
}
