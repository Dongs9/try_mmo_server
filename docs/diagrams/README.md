# 서버 다이어그램

코드를 바꿀 때 원본과 그림을 같은 커밋에서 갱신합니다.

- [구조](structure.md): 주요 클래스 관계
- [흐름](flow.md): 접속, 틱, 패킷 처리, 스레드

| 폴더 | 내용 |
| --- | --- |
| `src/` | Mermaid 원본. 수정은 여기서 합니다 |
| `images/` | 변환된 `.svg`, `.png`. 직접 고치지 않습니다 |
| `tools/` | 변환 도구. 클라이언트 저장소와 같습니다 (mermaid 11.4.1, Chrome 헤드리스) |

변환은 로컬에서만 이루어지며, 코드를 외부 서비스로 보내지 않습니다.

```bash
powershell -ExecutionPolicy Bypass -File docs\diagrams\tools\render.ps1
```
