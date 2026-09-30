# OwnEngine

AI가 쉽게 접근하고 검증할 수 있도록 설계한 C++17 게임 엔진입니다. 사람용 웹 에디터와 AI용 인터페이스(CLI · HTTP · MCP)가 **같은 명령 API**를 공유합니다.

![웹 에디터 — Showcase 샘플](docs/images/editor.png)

| 게임 화면 (`oe render samples/Showcase`) | |
|---|---|
| ![Showcase 렌더](docs/images/showcase.png) | glTF 여우 캐릭터(플레이어 조작 + 팔로우 카메라), 절차 생성 텍스처, 그림자, 포인트 라이트. 결정적 소프트웨어 렌더러의 출력(`oe render`)이며, 게임 창·웹·에디터 뷰포트는 같은 장면을 GPU 렌더러로 그립니다. |

- 별도 설치할 의존성 없음 (MSVC + CMake만 필요, Lua·Jolt Physics·sokol_gfx·stb·cgltf는 소스 동봉). 웹 런타임도 `runtime/web/`에 미리 빌드되어 포함 — 웹 배포에 Emscripten 불필요
- Windows(네이티브 창, `Name.exe` 패키징) · Web(WebAssembly + WebGL2, `oe package --web`) · 헤드리스 지원. Android / iOS / macOS / 콘솔은 플랫폼 계층만 추가하면 되도록 분리 — [docs/PLATFORMS.md](docs/PLATFORMS.md)
- 렌더러 두 개가 같은 장면을 그림: **GPU 렌더러**(sokol_gfx — Windows D3D11, Web WebGL2, Linux GLES3; 4× MSAA, 필터링된 그림자, 밉맵, 셰이더는 `engine/render/shaders/Shaders.glsl` 하나)는 게임 창·웹·에디터 뷰포트용, **소프트웨어 렌더러**(멀티스레드, 결정적)는 스크린샷 해시·테스트·피킹용
- 렌더링 기능: glTF 모델, PNG/JPEG 텍스처, 스무스/플랫 셰이딩, 포인트 라이트, 그림자, 직교/팔로우 카메라, 디버그 드로잉 — [docs/RENDERING.md](docs/RENDERING.md)
- 머티리얼/PBR/반투명: metallic-roughness(GGX) 셰이딩, 노멀맵·AO·발광 텍스처, 반투명(뒤에서 앞으로 정렬)·마스크·양면, glTF 머티리얼 완전 로드, `.mat.json` 머티리얼 파일(`material.create`/`material.set`, 핫리로드) — [docs/RENDERING.md](docs/RENDERING.md)
- Jolt 기반 3D 물리 (강체, 트리거, 캐릭터 컨트롤러, 결정적 시뮬레이션) — [docs/PHYSICS.md](docs/PHYSICS.md)
- 게임 UI: TrueType 폰트(한글 등 모든 언어, 폰트 파일 추가), 앵커·스트레치·부모-자식 배치, 레이아웃(세로/가로/그리드, 크기 맞춤), 리치 텍스트·줄바꿈·외곽선·그림자, 둥근 모서리·테두리 패널, 버튼 상태(호버/눌림/비활성), 이미지(9-slice, 채우기 바), 슬라이더/진행 바, 클리핑, 캔버스 스케일 — [docs/UI.md](docs/UI.md)
- 2D 게임: 스프라이트·스프라이트 시트 애니메이션(픽셀 아트, 투명 컷아웃), 텍스트로 쓰는 타일맵(충돌 자동 생성), XY 평면 물리, 경계 있는 카메라 추적, 에디터 2D 뷰 — [docs/2D.md](docs/2D.md)
- 게임 구성 요소: 프리팹, 씬 전환 + 게임 데이터, 메시지/타이머, 게임 내 UI, 오디오(결정적 믹서, 효과음 생성) — [docs/GAMEPLAY.md](docs/GAMEPLAY.md)
- Lua 5.4 스크립팅 (샌드박스, 핫리로드, 에러에 파일:줄 표시) — [docs/SCRIPTING.md](docs/SCRIPTING.md)
- 결정적(deterministic) 시뮬레이션과 소프트웨어 렌더러: 같은 입력이면 같은 프레임 해시 → AI가 테스트 오라클로 사용 가능

## 빌드

```bat
build.bat
build\bin\oe_tests.exe
```

Visual Studio 2022(“C++를 사용한 데스크톱 개발”)만 있으면 됩니다. CMake/Ninja는 VS에 포함된 것을 자동으로 사용합니다.

웹 런타임(`oe package --web`용)은 `runtime/web/`에 미리 빌드되어 들어 있어 따로 준비할 것이 없습니다. 엔진 C++을 고친 뒤 웹 빌드에도 반영하려면 [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)를 설치하고 `set EMSDK=C:\path\to\emsdk` 후 `build_web.bat` (Linux/macOS: `./build_web.sh`) — 결과가 `build\bin\web\`와 `runtime\web\`에 들어가니 함께 커밋하세요.

## 사용법

```bat
build\bin\oe.exe new MyGame                 :: 2스테이지 코인 수집 샘플 게임 프로젝트 생성
build\bin\oe.exe editor MyGame              :: 웹 에디터 (http://127.0.0.1:7777)
build\bin\oe.exe run MyGame                 :: 네이티브 창에서 플레이 (WASD / Space)
build\bin\oe.exe render MyGame --out shot.png --frames 60
build\bin\oe.exe exec MyGame scene.summary
build\bin\oe.exe exec MyGame entity.create "{\"name\":\"Box\",\"components\":{\"MeshRenderer\":{\"color\":\"#ff8800\"}}}" --save
build\bin\oe.exe import MyGame model.glb     :: 외부 모델/텍스처/사운드를 프로젝트로 복사
build\bin\oe.exe package MyGame             :: 배포용 폴더 dist\MyGame\ 생성 (MyGame.exe + game\)
build\bin\oe.exe package MyGame --web       :: 웹 배포 폴더 dist\MyGame-web\ (index.html + wasm, 정적 호스팅 어디든)
build\bin\oe.exe serve dist\MyGame-web      :: 웹 빌드를 로컬에서 실행 (http://127.0.0.1:8080)
build\bin\oe.exe api --markdown             :: 명령 레퍼런스 출력
```

## 에디터

- 계층(Hierarchy) · 뷰포트(궤도/팬/줌, 클릭 선택, F 포커스) · 인스펙터(리플렉션으로 자동 생성) · 콘솔(API 직접 호출)
- 뷰포트는 엔진이 GPU로 렌더링한 화면을 WebSocket으로 스트리밍(JPEG, 고해상도 디스플레이 대응). GPU가 없으면 소프트웨어 렌더러 + PNG로 자동 전환, 우측 상단 배지에 현재 렌더러 표시
- Play / Pause / Step / Stop(씬 복원), Undo / Redo, 저장
- 2D 뷰(타일맵이 있는 씬은 자동): 정면 보기, 드래그로 이동, 휠 확대. JSON 필드(타일맵 map 등)는 여러 줄 텍스트로 편집
- Game 뷰에서 플레이 중 키 입력이 엔진으로 전달됨
- 다른 도구(AI 에이전트 등)가 씬을 바꾸면 에디터에 실시간 반영 (바뀐 필드는 인스펙터에서 잠깐 강조)
- 계층 검색(이름·컴포넌트), 접기/펼치기, 키보드 이동(↑↓←→, Del, F, F2, Ctrl+D)
- 인스펙터: 컴포넌트 접기, 라벨·X/Y/Z 드래그로 값 조절(Shift 미세, Ctrl 크게 — 드래그 한 번 = Undo 한 단계), 범위 있는 값은 슬라이더
- 패널 경계 드래그로 크기 조절(더블클릭 초기화), A−/A+ 로 UI 크기, 콘솔 레벨 필터(Info/Warnings/Errors)
- 단축키: Ctrl+S 저장, Ctrl+Z/Y 되돌리기, Ctrl+P 플레이/정지

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
engine/script    Lua 스크립트 호스트 (ScriptHost)
engine/physics   물리 (Jolt 래퍼, PhysicsWorld)
engine/audio     오디오 믹서, WAV, 효과음 생성기
engine/assets    에셋 관리, glTF/이미지 로딩
engine/render    IRenderer, 소프트웨어 래스터라이저, GPU 렌더러(sokol_gfx) + 셰이더, 메시, 게임 UI
engine/api       명령 레지스트리, HTTP 서버, 에디터 라우트, MCP 서버
engine/app       Engine (시뮬레이션, undo, 작업 큐), 프로젝트 템플릿
engine/platform  Platform.h + win32(D3D11) / web(WebGL2) / null(EGL) 구현
editor/          웹 에디터 (HTML/CSS/JS)
tools/oe         CLI
tools/player     게임 런타임 (Name.exe / 웹 wasm) + 웹 페이지 템플릿
tools/shaders    셰이더 재생성 스크립트 (sokol-shdc)
tests/           자체 테스트
third_party/lua  Lua 5.4.8 (MIT)
third_party/jolt Jolt Physics 5.6.0 (MIT)
third_party/stb, cgltf  이미지 디코더·인코더, glTF 디코더 (PD/MIT, MIT)
third_party/sokol  sokol_gfx (zlib) — D3D11 / WebGL2 / GLES3 추상화
templates/       `oe new` 프로젝트 템플릿
samples/Hello    샘플 프로젝트 (템플릿으로 생성)
samples/Showcase 렌더링 샘플 (glTF 여우 캐릭터, 텍스처, 그림자, 포인트 라이트)
samples/Platformer 2D 횡스크롤 플랫포머 (텍스트 타일맵, 스프라이트 애니메이션, 적, ? 블록, 패럴랙스)
samples/FPS      1인칭 슈팅 테스트 게임 (마우스 시점, 히트스캔 권총, 재장전, 움직이는 표적, 결과 화면)
```
