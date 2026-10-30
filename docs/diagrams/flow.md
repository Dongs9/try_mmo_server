# 서버 흐름

## 접속과 입장

![접속과 입장](images/flow-connect.svg)

## 틱

![틱 루프](images/flow-tick.svg)

## 패킷 처리

TCP 스트림을 프레임으로 자르는 일은 `TcpTransport`가 합니다.

![프레임 분리](images/flow-packet.svg)

완성된 프레임의 검증과 처리는 `GameServer`가 합니다.

![요청 처리](images/flow-request.svg)

## 스레드

서버 로직은 스레드 하나에서 돌아갑니다. 콘솔 종료 신호만 다른 스레드에서 들어옵니다.

![스레드](images/flow-thread.svg)

## 플레이어 행동과 공격

클라이언트는 대상만 지정합니다. 접근과 타격은 서버가 틱마다 처리합니다.

![플레이어 행동](images/flow-attack.svg)

## 보상과 레벨업

![보상과 레벨업](images/flow-reward.svg)

## 슬라임 상태

![슬라임 상태](images/flow-slime.svg)
