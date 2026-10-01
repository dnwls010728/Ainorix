# OwnEngine 기능 개발 로드맵

기능 개발 위주 순서. 앞 단계가 뒤 단계의 기반이 되도록 의존성 순으로 배치했다.
각 기능은 명령 API로 노출하고, 가능한 한 AI가 결과를 직접 확인할 수 있게 만든다 (스크린샷, 조회 명령).

## 기능 하나를 추가하는 흐름

```
1. 설계     : 컴포넌트 / 명령 정의 (이름, 인자 스키마, 에러 코드 + hint)
2. 구현     : Components.h / Systems.cpp / Commands.cpp
3. 테스트   : tests/tests.cpp 에 케이스 추가
4. 확인     : oe render ... --out shot.png → 이미지 확인
5. 문서     : oe api --markdown > docs/API.md
6. 에디터   : 리플렉션으로 대부분 자동 반영, 전용 UI가 필요할 때만 engine/editor/ 수정
```

---

## 1단계 — 게임 로직: 스크립팅 ✅ 완료

C++ 재빌드 없이 게임 로직을 작성/수정할 수 있어야 이후 모든 기능이 쓸모를 가진다.

- Lua 5.4 내장 (소스 동봉, 외부 설치 불필요)
- `Script` 컴포넌트: `onStart`, `onUpdate(dt)`, `onCollision(other)` 콜백
- 스크립트에서 쓸 엔진 API: 엔티티 조회/생성/삭제, 컴포넌트 읽기/쓰기, 입력, 로그
- 파일 변경 시 핫리로드
- 명령: `script.eval` (즉석 실행), `script.reload`
- 스크립트 에러는 파일:줄 + 메시지로 로그와 API 에러에 표시

**완료 기준**: 기존 `Rotator` / `PlayerController`를 Lua 스크립트로 똑같이 구현 가능

## 2단계 — 충돌과 물리 ✅ 완료 (Jolt Physics)

- `Collider` 컴포넌트: Box, Sphere (트리거 여부 옵션)
- `RigidBody` 컴포넌트: 속도, 중력, 질량, 고정 여부
- 충돌 감지 + 간단한 반발/밀어내기, 트리거 진입/이탈 이벤트 → 스크립트 콜백
- 명령: `physics.raycast`, `physics.overlap`
- 에디터: 콜라이더 와이어프레임 표시

**완료 기준**: 플레이어가 벽에 막히고, 코인에 닿으면 이벤트 발생

추가 ✅ **2D 물리 (Box2D 3.1)**: `Collider2D`(박스·원·캡슐·다각형·엣지/루프, 원웨이, 충돌 레이어 16개) / `RigidBody2D` / `CharacterBody2D`(플랫포머·탑뷰, 경사로, 움직이는 발판, 밀기). Jolt 월드와 이벤트·쿼리를 합쳐서 제공. 타일셋 파일 + 자동 연결(16/47 패턴) + 타일별 충돌(원웨이·슬로프), 에디터 타일 브러시 — [2D.md](2D.md)

## 3단계 — 게임 구성 요소 ✅ 완료

- **프리팹**: 엔티티 묶음을 파일로 저장/인스턴스화, 필드 오버라이드 (`prefab.create`, `prefab.instantiate`)
- **씬 전환**: 스크립트에서 다른 씬 로드, 씬 간 유지되는 데이터
- **게임 내 UI**: 텍스트, 이미지, 버튼 (화면 좌표 기준) — 점수/HUD용
- **오디오**: 효과음 재생, 배경음 루프, 볼륨 (WAV부터)
- **타이머/이벤트**: 지연 실행, 엔티티 간 메시지

**완료 기준**: 샘플 "코인 수집 게임" 완성 — 이동, 충돌, 점수 UI, 효과음, 클리어 시 다음 씬

## 4단계 — 에셋과 렌더링 품질 ✅ 완료

- **에셋 관리**: `assets/` 폴더, 에셋 목록/조회 명령 (`asset.list`, `asset.info`)
- **메시 임포트**: glTF (.glb)
- **텍스처**: PNG 디코더, UV 매핑, 머티리얼 컴포넌트 (색 + 텍스처)
- **셰이딩**: 스무스 셰이딩(정점 노멀), 포인트 라이트, 간단한 그림자
- **카메라**: 직교 투영, 카메라 추적(팔로우) 컴포넌트
- **디버그 드로잉**: 선/박스/구를 스크립트·API에서 그리기

**완료 기준**: 외부 glTF 캐릭터 모델이 텍스처와 함께 표시

## 5단계 — 에디터 기능 (네이티브 에디터 ✅)

`oe editor`가 **네이티브 에디터**(Dear ImGui 도킹 + ImGuizmo, 엔진과 같은 프로세스, GPU 텍스처 뷰포트)를 엽니다. 웹 에디터는 제거했습니다. 자세한 내용: [EDITOR.md](EDITOR.md)

- ~~뷰포트 기즈모: 이동 / 회전 / 스케일~~ ✅ (Q/W/E/R, 로컬/월드, 스냅)
- ~~하이어라키: 드래그로 부모 변경, 다중 선택~~ ✅ (복제 Ctrl+D; 클립보드 복사/붙여넣기는 미구현)
- ~~에셋 브라우저: 에셋 목록, 드래그로 씬에 배치~~ ✅ (+ 창에 파일 드롭으로 가져오기)
- ~~스크립트 편집기 (코드 에디터 + 에러 표시)~~ ✅ (문법 강조, 줄 번호, 찾기/바꾸기, 입력 중 검사)
- ~~AI 작업 표시: 에이전트가 수정한 엔티티 하이라이트~~ ✅ (알림 + 계층 표시; 변경 이력 패널은 미구현)
- 프리팹 편집 모드
- 네이티브 파일 대화상자, 에셋 썸네일, 여러 창(ImGui 멀티 뷰포트)
- macOS 네이티브 에디터: AppKit 창(이벤트 모드) + Metal `GpuDevice` — 에디터 코드는 그대로 동작 ([PLATFORMS.md](PLATFORMS.md))

## 6단계 — 고급 기능

- 애니메이션: 트랜스폼 키프레임, glTF 스켈레탈 애니메이션
- 파티클 시스템
- ~~하드웨어 렌더러~~ ✅ sokol_gfx 기반 `GpuRenderer` (Windows D3D11, Web WebGL2, Linux GLES3) — 소프트웨어 렌더러와 같은 인터페이스
- GPU 효과: 커스텀 셰이더 머티리얼, 포스트 프로세싱(톤매핑, 블룸, FXAA …) — composite 패스가 연결 지점 ([RENDERING.md](RENDERING.md))
- 내비게이션 (그리드 기반 길찾기)

## 7단계 — 플랫폼 확장

기능이 어느 정도 갖춰진 뒤 진행. 비용 낮은 순서:

1. ~~Web (Emscripten)~~ ✅ WebGL2 + WebAudio, `oe package --web`
2. macOS → iOS (Metal) — 계획 및 작업 로그: [APPLE.md](APPLE.md) (Mac 없이 GitHub Actions macOS 러너 + rcodesign으로 진행)
3. ~~Android (NDK, 터치 입력)~~ ✅ NativeActivity + GLES3 + AAudio, `oe package --android` (APK 생성·서명) — 실기기 검증 남음, [ANDROID.md](ANDROID.md)
4. Linux 데스크톱
5. 콘솔 (Switch / PlayStation) — SDK 확보 후

자세한 이식 방법은 [PLATFORMS.md](PLATFORMS.md) 참고.

---

## 바로 다음 작업 (우선순위 순, 미구현 우선)

1~4단계 완료 (스크립팅, 물리, 게임 구성 요소, 에셋·렌더링 + 샘플). GPU 렌더러(D3D11 / WebGL2 / GLES3), 웹 패키징, Android 패키징, UI 개편(TTF 한글 폰트, 레이아웃), 2D(Box2D 물리, 타일셋·자동 연결, 타일 브러시), PBR 머티리얼, 네이티브 에디터 완료.

**에이전트 규칙**: 아래 목록에서 **체크되지 않은 첫 항목**부터 시작한다. 시작 전에 [DESIGN.md](DESIGN.md)(불변 조건, 완료 기준, 인계 §5)를 읽는다. 항목마다 해당 기능 문서(없으면 새로 만들기)에 `Implementation status (work log)` 체크리스트를 먼저 만들고(예: [ANDROID.md](ANDROID.md)), 마일스톤마다 커밋·푸시하며 그 줄을 체크한다. 항목 하나 = 브랜치/PR 하나. 끝나면 이 목록의 `[ ]`를 `[x]`로 바꾸고, 하지 못한 검증은 줄을 새로 만들어 남긴다. 순서를 바꿔야 하면 이유를 이 문서에 적는다.

### P1. 세이브 데이터 영구 저장 — [ ]

- **왜**: `game.get/set`은 플레이 세션 동안만 유지된다(`Engine::GameData()`, `engine/app/Engine.h`). 종료하면 사라져 출시 게임이 진행 상황을 저장할 수 없다. 작고 독립적이며 이후 모든 플랫폼에 영향을 주므로 가장 먼저 한다.
- **요구**: 스크립트 API `save.get(key, default)`, `save.set(key, value)`, `save.delete(key)`, `save.flush()`(또는 `game.save`/`game.load`처럼 기존 이름과 일관되게 정하고 `docs/SCRIPTING.md`에 문서화). 값은 JSON 직렬화 가능한 것만. 별도 슬롯 지원(`slot` 이름).
- **저장 위치**: 데스크톱 = 사용자 데이터 폴더(Windows `%APPDATA%/<게임이름>/`)의 JSON 파일(임시 파일에 쓴 뒤 rename, 깨진 파일은 무시하고 로그), 웹 = `localStorage`(Emscripten JS 연결), Android = 앱 내부 저장소 `ANativeActivity::internalDataPath`. 경로 접근은 `Platform.h`에 함수를 추가(`PlatformSaveDirectory()` 등)하고 `engine/platform/*`에만 OS 코드를 둔다.
- **결정성**: 시뮬레이션은 결정적이어야 한다. `oe render`, 테스트, `oe script`/MCP 세션은 기본적으로 **메모리 저장소**(디스크에 쓰지 않음)를 쓰고, 플레이어 런타임과 `oe` 명령 `--save-dir`로만 디스크를 쓴다.
- **명령**: `save.state`(읽기), `save.set`/`save.clear`(수정, 테스트용). `docs/API.md` 재생성.
- **테스트**: 쓰기→재시작→읽기, 손상된 파일, 슬롯, 메모리 모드에서 디스크 무접촉, `TemplateGamePlaythrough` 유지. 템플릿에 최고 점수 저장을 하나 넣으면 좋다(템플릿 변경 시 `samples/Hello` 재생성).
- **검증하지 못할 수 있는 것**: 웹 localStorage는 `node build-web/bin/oe_tests.js`로 못 보면 브라우저(Playwright)로 확인하고, 웹 런타임 재빌드(`runtime/web/`)는 Emscripten이 있을 때만.

P1 is implemented in PR #15, pending review. P2 is developed independently from
main in PR #16 because it has no save-data dependency; this preserves one feature
per PR while P1 awaits review. P1's checkbox remains on its own feature branch.

### P2. 스켈레탈 애니메이션 (glTF) — [x]

- Implementation and verification: [ANIMATION.md](ANIMATION.md). Windows 67 tests,
  Node/WASM 63 tests, D3D11/software animated comparison and browser WebGL2 checked;
  web and both Android ABI runtimes refreshed.
- [ ] Android physical-device and full Linux/EGL execution remain unverified;
  see the feature work log for the exact follow-up steps.

- **왜**: 스킨 모델은 지금 bind pose로만 나온다(`docs/RENDERING.md`, `engine/assets/Assets.cpp`의 `node->skin` 처리). 3D 캐릭터가 움직이지 못하는 가장 큰 공백이다.
- **요구**: glTF `skins`(joints, inverseBindMatrices, JOINTS_0/WEIGHTS_0)와 `animations`(translation/rotation/scale 채널, LINEAR/STEP, 가능하면 CUBICSPLINE) 로딩. 컴포넌트 `Animator {clip, speed, loop, playing, time}`(리플렉션 → 직렬화/API/인스펙터 자동). 스크립트 `entity:play(clip)` 류와 클립 목록 조회(`asset.info`에 `clips`). 크로스페이드는 2차(선택).
- **구현 위치**: 포즈 계산은 `engine/scene/Systems.cpp`(고정 스텝, 결정적), 스킨 행렬 팔레트는 `RenderScene`에 담아 **두 렌더러(`SoftwareRenderer`, `GpuRenderer` + `Shaders.glsl`)에 같은 방식으로** 적용한다. 셰이더를 고치면 `tools/shaders/compile_shaders.*`로 `Shaders.glsl.h` 재생성(sokol-shdc 필요; 없으면 그 사실을 남기고 CPU 쪽부터). GPU 경로는 정점 셰이더 스키닝, 조인트 수 상한(예: 64)을 문서화한다.
- **검증**: `GpuRendererMatchesSoftware`가 스킨 모델로도 통과, 같은 시간에서 프레임 해시 고정, `render.screenshot`로 걷는 샘플 확인. `samples/Showcase`의 여우(Fox)에 Survey/Walk/Run 클립을 연결해 데모로 쓴다.
- **주의**: 성능(스킨 정점 수 × 소프트웨어 렌더러) 때문에 테스트 장면은 작게. 멀티스레드 렌더러의 결정성을 깨지 말 것.

### P3. 파티클 시스템 — [ ]

- **왜**: 코드에 파티클이 전혀 없다. 이펙트(폭발, 먼지, 코인 반짝임)가 없어 게임 느낌이 약하다.
- **요구**: 컴포넌트 `ParticleEmitter {rate, burst, lifetime, speed, spread, gravity, startSize/endSize, startColor/endColor, texture/frame, space(local|world), maxParticles, loop, playing}`. 2D(스프라이트 빌보드)와 3D 모두. 스크립트 `emitter:burst(n)`.
- **결정성(핵심)**: 난수는 엔티티 id + 고정 시드의 자체 PRNG(시뮬레이션 프레임 기준)로 한다. `std::rand`, 시간, 포인터 순서 금지. 같은 씬 + 같은 입력 = 같은 프레임 해시.
- **구현 위치**: 시뮬레이션 `Systems.cpp`, 그리기는 `BuildDrawList`가 빌보드 쿼드를 만들어 양쪽 렌더러가 동일하게 그리게(투명 정렬 규칙 따르기). 에디터에서 재생/정지 미리보기는 2차.
- **검증**: `render.screenshot --frames N` 해시 테스트, 입자 수 상한 테스트, `samples/Platformer`/`Dungeon`에 코인·피격 이펙트 적용.

### P4. 게임패드 입력 일반화 — [ ]

- **왜**: Android에만 게임패드가 있고(`ANDROID.md`: D-pad/왼 스틱 = 방향키) 데스크톱·웹에는 없다. 아날로그 축도 없다.
- **요구**: `InputState`에 축(`axes`: `LeftX/LeftY/RightX/RightY/LT/RT`)과 버튼 이름(`GamepadA/B/X/Y/LB/RB/Start/Back/DPadUp...`)을 추가하고 Lua `input.axis(name)`, 기존 `input.key`로도 버튼 접근. API `input.axis {name, value}`로 테스트 주입(`input.touch`처럼). 데드존 처리는 엔진에서(기본 0.15, 문서화).
- **플랫폼**: Windows = XInput(동적 로드, 없으면 무시), 웹 = Gamepad API(`navigator.getGamepads`), Android는 기존 매핑에 축 추가. 새 플랫폼 파일에는 OS 코드를 넣고 엔진 코드는 이식 가능하게 유지.
- **검증**: 입력 주입 테스트, `PlatformNull`로 컴파일 확인. 실제 컨트롤러는 장치가 있어야 하므로 못 하면 체크리스트에 남긴다.

### P5. 웹 멀티터치 `input.touches` — [ ]

- `engine/platform/web/PlatformWeb.cpp`의 `OnTouch`가 첫 손가락만 마우스로 매핑한다. 모든 `EmscriptenTouchPoint`를 `InputState.touches`에 채운다(`began` 포함, 첫 손가락은 계속 마우스로). 웹 런타임 재빌드 필요(Emscripten SDK 있을 때), 없으면 소스만 고치고 미검증으로 남긴다. 작은 작업이라 P1~P4 사이에 끼워도 된다.

### P6. GPU 효과 (셰이더 머티리얼, 포스트 프로세싱) — [ ]

- composite 패스(`GpuRenderer`)가 연결 지점. 톤매핑, 블룸, FXAA, 비네트를 켜고 끄는 컴포넌트/카메라 설정부터. **소프트웨어 렌더러는 기준**이므로 효과가 해시에 영향을 주면 안 되게 기본값은 꺼짐으로 두거나, 양쪽 모두 구현한다(`CLAUDE.md` 규칙). 커스텀 셰이더 머티리얼은 sokol-shdc 의존이 커서 이 항목의 후반부로 둔다.

### P7. 에디터 보강 — [ ]

- 하이어라키 복사/붙여넣기(클립보드), 변경 이력(undo 이력) 패널, 프리팹 편집 모드, 에셋 썸네일, 네이티브 파일 대화상자. 편집은 반드시 `Impl::Call`(명령)로, UI 문자열은 `Tr("...")` + `EditorText.cpp`에 한국어·일본어 추가, `oe editor --screenshot`으로 확인, `NativeEditorHeadless` 테스트 추가([EDITOR.md](EDITOR.md)).

### P8. 내비게이션(그리드 길찾기) — [ ]

- `Tilemap`의 솔리드 셀을 쓰는 A*(`tilemap.path {from,to}` 명령 + Lua `nav.path`). 결정적 타이브레이크(셀 인덱스 순). 2D 샘플의 슬라임 AI에 적용.

### P9. 개발 도구 제외 빌드 (`OE_ENABLE_DEVTOOLS`) — [ ]

- 출시 빌드에서 HTTP 서버/MCP를 컴파일 제외하는 CMake 옵션(`docs/PLATFORMS.md` 참고). 콘솔/스토어 출시 전에 필요. 플레이어 타깃 기본값을 먼저 정하고 크기/보안 차이를 문서화한다.

### 플랫폼 트랙 (기능 작업과 병행 가능)

- **macOS → iOS**: [APPLE.md](APPLE.md)의 작업 로그를 따른다. 첫 항목은 macOS CI 워크플로. (P1·P4의 플랫폼 함수가 정해진 뒤에 플랫폼 코드를 쓰면 다시 손댈 일이 줄어든다.)
- **Linux 데스크톱**: 헤드리스 GLES가 이미 있다. X11/Wayland(또는 SDL3) 창 + 입력 + 오디오만 추가([PLATFORMS.md](PLATFORMS.md) 체크리스트).
- **검증 대기(구현은 끝남)**: Android 실기기(멀티터치, 스피커/헤드폰, 16KB 페이지 기기) — [ANDROID.md](ANDROID.md) 미체크 줄. 에뮬레이터 SwiftShader 검은 화면 조사.
