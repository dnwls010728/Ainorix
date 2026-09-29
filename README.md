# OwnEngine

AI가 쉽게 접근하고 검증할 수 있도록 설계한 C++17 게임 엔진입니다. 사람용 웹 에디터와 AI용 인터페이스(CLI · HTTP · MCP)가 **같은 명령 API**를 공유합니다.

- 외부 의존성 없음 (MSVC + CMake만 필요)
- 현재 Windows 지원 (네이티브 창 + 헤드리스). Web / Android / iOS / macOS / 콘솔은 플랫폼 계층만 추가하면 되도록 분리 — [docs/PLATFORMS.md](docs/PLATFORMS.md)
- 결정적(deterministic) 시뮬레이션과 소프트웨어 렌더러: 같은 입력이면 같은 프레임 해시 → AI가 테스트 오라클로 사용 가능

## 빌드

```bat
build.bat
build\bin\oe_tests.exe
```

Visual Studio 2022(“C++를 사용한 데스크톱 개발”)만 있으면 됩니다. CMake/Ninja는 VS에 포함된 것을 자동으로 사용합니다.

## 사용법

```bat
build\bin\oe.exe new MyGame                 :: 샘플 씬이 들어있는 프로젝트 생성
build\bin\oe.exe editor MyGame              :: 웹 에디터 (http://127.0.0.1:7777)
build\bin\oe.exe run MyGame                 :: 네이티브 창에서 플레이 (WASD / Space)
build\bin\oe.exe render MyGame --out shot.png --frames 60
build\bin\oe.exe exec MyGame scene.summary
build\bin\oe.exe exec MyGame entity.create "{\"name\":\"Box\",\"components\":{\"MeshRenderer\":{\"color\":\"#ff8800\"}}}" --save
build\bin\oe.exe api --markdown             :: 명령 레퍼런스 출력
```

## 에디터

- 계층(Hierarchy) · 뷰포트(궤도/팬/줌, 클릭 선택, F 포커스) · 인스펙터(리플렉션으로 자동 생성) · 콘솔(API 직접 호출)
- Play / Pause / Step / Stop(씬 복원), Undo / Redo, 저장
- Game 뷰에서 플레이 중 키 입력이 엔진으로 전달됨
- 다른 도구(AI 에이전트 등)가 씬을 바꾸면 에디터에 실시간 반영

## AI 연동

| 방식 | 명령 |
|---|---|
| MCP (Claude Code 등) | `oe mcp <project> [--port 7777]` — 모든 명령이 MCP 도구, 스크린샷은 이미지로 반환. `--port`를 주면 사람이 같은 세션을 에디터로 봄 |
| 실행 중인 에디터에 붙기 | `oe mcp --connect 7777` |
| CLI / 스크립트 | `oe exec`, `oe script` (JSON 입출력, 실패 시 exit code 1) |
| HTTP | `POST /api/call {"command": "...", "args": {...}}` |

이 저장소의 [.mcp.json](.mcp.json)은 Claude Code에서 `samples/Hello` 프로젝트를 MCP 서버로 연결합니다 (먼저 빌드 필요). 에이전트용 작업 가이드는 [CLAUDE.md](CLAUDE.md) / [AGENTS.md](AGENTS.md), 전체 API는 [docs/API.md](docs/API.md), 구조는 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)에 있습니다.

## 구조

```
engine/core      Json, Math, Log, PNG, FileSystem
engine/scene     리플렉션, 컴포넌트, Scene, 시스템
engine/render    IRenderer, 소프트웨어 래스터라이저, 메시
engine/api       명령 레지스트리, HTTP 서버, 에디터 라우트, MCP 서버
engine/app       Engine (시뮬레이션, undo, 작업 큐), 프로젝트 템플릿
engine/platform  Platform.h + win32 / null 구현
editor/          웹 에디터 (HTML/CSS/JS)
tools/oe         CLI
tests/           자체 테스트
samples/Hello    샘플 프로젝트
```
