# 통신 프로토콜

메시지 원본은 [proto](../proto) 폴더입니다.

| 파일 | 내용 |
| --- | --- |
| [common.proto](../proto/common.proto) | 공용 타입: `Vec2`, `EntityType`, `EntityState` |
| [packet.proto](../proto/packet.proto) | `MsgId`와 클라이언트↔서버 메시지 전부. `common.proto`만 import |

서버 내부 메시지가 필요해지면 `server.proto`를 추가합니다.

## 코드 생성

서버와 클라이언트 모두 같은 `protoc`를 씁니다. 버전은 `libprotoc 36.2`입니다.

```text
vcpkg_installed\x64-windows-static-md\tools\protobuf\protoc.exe
```

C++ 코드는 `try_mmo_proto` 프로젝트를 빌드하면 생성됩니다. `.proto` 파일을 하나씩 따로 생성하지 말고 두 파일을 함께 생성하세요.

## 메시지 번호

| 구간 | 메시지 | 번호 |
| --- | --- | --- |
| 입장 | `C2S_ENTER_REQ` | 1000 |
| 입장 | `S2C_ENTER_RES` | 1001 |
| 월드 | `C2S_MOVE_REQ` | 2000 |
| 월드 | `S2C_ENTITY_SPAWN` | 2001 |
| 월드 | `S2C_ENTITY_DESPAWN` | 2002 |
| 월드 | `S2C_POSITION_SYNC` | 2003 |

입장은 1000번대, 월드는 2000번대를 씁니다.

## 프레임

TCP 스트림 위에 프레임을 이어 보냅니다. 정수는 모두 리틀 엔디언입니다.

| 위치 | 크기 | 내용 |
| --- | --- | --- |
| 0 | 2바이트 | `length`: 이 필드를 뺀 나머지 크기 (메시지 ID + 본문) |
| 2 | 2바이트 | `msg_id`: `MsgId` 값 |
| 4 | `length - 2`바이트 | Protobuf 본문. 필드가 모두 기본값이면 0바이트일 수 있음 |

`length`는 최소 2입니다. 최대값은 방향마다 다릅니다.

| 방향 | 최대 `length` | 초과 시 |
| --- | --- | --- |
| 클라이언트 → 서버 | 8192 | 잘못된 프레임으로 보고 서버가 연결을 끊음 |
| 서버 → 클라이언트 | 65535 (필드 크기 한도) | 서버 내부 오류. 클라이언트는 이 크기까지 받아야 함 |

8192는 입력 메시지가 작다는 점을 이용한 서버 보호 장치이므로 서버 수신에만 적용합니다. 서버가 보내는 스냅샷은 개체 수에 비례해 커집니다.

## 흐름

1. 클라이언트가 접속 후 `EnterReq`를 보냅니다.
2. 서버가 `EnterRes`로 답합니다. 존이 100명이면 `ZONE_FULL`이고 연결은 유지됩니다.
3. 이후 서버는 자기 자신을 뺀 AOI 안 개체마다 `EntitySpawn`을 보내고, 틱마다 `PositionSync`를 보냅니다.
4. 자기 자신은 `EntitySpawn`으로 오지 않고 `EnterRes.self`로만 받습니다. `PositionSync`에는 자기 자신도 들어 있습니다.
5. 같은 틱의 `EntitySpawn`은 `PositionSync`보다 먼저 도착합니다.

## AOI

플레이어 기준 중심 거리로 판정합니다. 거리 700 이하면 등장, 800을 넘으면 퇴장입니다.
