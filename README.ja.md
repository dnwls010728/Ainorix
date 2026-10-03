# OwnEngine

[한국어](README.md) | [English](README.en.md) | **日本語**

AI が人と同じように扱い、検証できるように設計した C++17 ゲームエンジンです。人のためのエディターと AI のためのインターフェース (CLI・HTTP・MCP) が**同じコマンド API** を共有します。

- エディタ機能: 階層クリップボード、変更履歴、プレハブのソース編集、アセットサムネイル、Windowsファイルダイアログ — [docs/EDITOR.md](docs/EDITOR.md)

![ネイティブエディター - Showcase サンプル](docs/images/native-editor.png)

| ゲーム画面 (`oe render samples/Showcase`) | |
|---|---|
| ![Showcase のレンダリング](docs/images/showcase.png) | glTF のキツネキャラクター (プレイヤー操作 + 追従カメラ)、手続き生成テクスチャ、影、ポイントライト。決定的なソフトウェアレンダラーの出力 (`oe render`) で、ゲームウィンドウ・Web・エディタービューは同じシーンを GPU レンダラーで描画します。 |

- 別途インストールする依存関係なし (MSVC + CMake のみ。Lua・Jolt Physics・Box2D・sokol_gfx・stb・cgltf・Dear ImGui・ImGuizmo はソースを同梱)。Web ランタイムも `runtime/web/` にビルド済みで同梱 - Web 配布に Emscripten は不要
- Windows (ネイティブウィンドウ、`Name.exe` パッケージ)・Web (WebAssembly + WebGL2、`oe package --web`)・Android (APK、`oe package --android` - [docs/ANDROID.md](docs/ANDROID.md))・ヘッドレスに対応。iOS / macOS / コンソールはプラットフォーム層を追加するだけで済むよう分離 - [docs/PLATFORMS.md](docs/PLATFORMS.md)
- 2 つのレンダラーが同じシーンを描画: **GPU レンダラー** (sokol_gfx - Windows は D3D11、Web は WebGL2、Linux は GLES3。4x MSAA、フィルタリングされた影、ミップマップ、シェーダーは `engine/render/shaders/Shaders.glsl` 1 つ) はゲームウィンドウ・Web・エディタービュー用、**ソフトウェアレンダラー** (マルチスレッド、決定的) はスクリーンショットのハッシュ・テスト・ピッキング用
- レンダリング: glTF モデル、PNG/JPEG テクスチャ、スムーズ/フラットシェーディング、ポイントライト、影、平行投影/追従カメラ、デバッグ描画 - [docs/RENDERING.md](docs/RENDERING.md)
- スケルタルアニメーション: glTF TRS・スキン、LINEAR/STEP/CUBICSPLINE、Animator/API/Lua 再生、ソフトウェア/GPU スキニング（最大64ジョイント）。Showcase のキツネは停止・移動・Shift ダッシュで Survey/Walk/Run を切り替えます - [docs/ANIMATION.md](docs/ANIMATION.md)
- パーティクル: 決定的な2D/3D放出、ローカル/ワールド空間、API/Lua burst、両レンダラーのカメラビルボード、寿命に応じたサイズ・色・透明度とシートフレーム。Platformer・Dungeonにコイン・被弾エフェクト - [docs/PARTICLES.md](docs/PARTICLES.md)
- マテリアル / PBR / 半透明: metallic-roughness (GGX) シェーディング、法線・AO・発光マップ、半透明 (奥から手前へソート)・マスク・両面、glTF マテリアルの完全読み込み、`.mat.json` マテリアルファイル (`material.create` / `material.set`、ホットリロード) - [docs/RENDERING.md](docs/RENDERING.md)
- Jolt による 3D 物理と Box2D による 2D 物理 (剛体、トリガー、キャラクターコントローラー、一方通行の足場・多角形・衝突レイヤー、決定的シミュレーション) - [docs/PHYSICS.md](docs/PHYSICS.md)
- ゲーム UI: TrueType フォント (あらゆる言語、フォントファイルを追加)、アンカー・ストレッチ・親子配置、レイアウト (縦/横/グリッド、内容に合わせたサイズ)、リッチテキスト・折り返し・アウトライン・影、角丸・枠付きパネル、ボタンの状態 (ホバー/押下/無効)、画像 (9-slice、フィルバー)、スライダー/プログレスバー、クリッピング、キャンバススケール - [docs/UI.md](docs/UI.md)
- 2D ゲーム: スプライト・スプライトシートアニメーション (ピクセルアート、透明の切り抜き)、テキストで書くタイルマップ (タイルセットファイル、16/47 パターンのオートタイル、ランダムなバリエーション、タイルごとの衝突: ソリッド・一方通行・坂)、Box2D の 2D 物理、範囲付き追従カメラ、エディターの 2D ビューとタイルブラシ - [docs/2D.md](docs/2D.md)
- ゲームの構成要素: プレハブ、シーン切り替え + ゲームデータ、メッセージ/タイマー、ゲーム内 UI、オーディオ (決定的ミキサー、効果音の生成) - [docs/GAMEPLAY.md](docs/GAMEPLAY.md)
- Lua 5.4 スクリプト (サンドボックス、ホットリロード、エラーにファイル:行を表示) - [docs/SCRIPTING.md](docs/SCRIPTING.md)
- カメラ後処理: ソフトウェア・GPUの露出・HDR Reinhardトーンマッピング・ブルーム・ビネット・FXAAに対応し、UIと選択輪郭を維持。Showcaseの1〜6キーでOff・Tone・Bloom・Vignette・FXAA・Allを選択。`shader.create`・`shader.check`でシェーダーグラフを作成・検証可能。マテリアルグラフをCPU・GPUのメインパスで実行。グラフのアルファを影・選択パスにも適用。追加GPU演算・パッケージ検証は進行中 - [docs/POSTPROCESS.md](docs/POSTPROCESS.md)
- ゲームパッド入力: 6軸のAPI注入、Lua `input.axis`、共通0.15デッドゾーン、Windows XInput・Web Gamepad API・Android対応、エディターGameビュー転送とPlatformer/Dungeon操作。実コントローラー検証は未完了 - [docs/INPUT.md](docs/INPUT.md)
- Webマルチタッチ: Lua `input.touches()`に全指を渡し、画面上のキーを同時操作。最初の指はマウスとしても動作。ブラウザー合成イベント検証済み、モバイル実機検証は未完了 - [docs/TOUCH.md](docs/TOUCH.md)
- 決定的 (deterministic) なシミュレーションとソフトウェアレンダラー: 同じ入力なら同じフレームハッシュ → AI がテストオラクルとして使えます

## ビルド

```bat
build.bat
build\bin\oe_tests.exe
```

Visual Studio 2022 (「C++ によるデスクトップ開発」) だけで十分です。CMake/Ninja は VS に同梱のものを自動で使います。

Web ランタイム (`oe package --web` 用) は `runtime/web/` にビルド済みで入っているため、準備は不要です。エンジンの C++ を変更して Web ビルドにも反映するには、[Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html) をインストールし、`set EMSDK=C:\path\to\emsdk` の後に `build_web.bat` (Linux/macOS: `./build_web.sh`) を実行します。結果は `build\bin\web\` と `runtime\web\` に出力されるので、一緒にコミットしてください。

## 使い方

```bat
build\bin\oe.exe new MyGame                 :: プロジェクトを作成 (2 ステージのコイン集めサンプルゲーム)
build\bin\oe.exe editor MyGame              :: エディター (エージェント用の API も http://127.0.0.1:7777 で同時に提供)
build\bin\oe.exe editor MyGame --lang ja    :: エディターの言語を指定 (ja / en / ko、既定は OS の言語)
build\bin\oe.exe run MyGame                 :: ネイティブウィンドウでプレイ (WASD / Space)
build\bin\oe.exe render MyGame --out shot.png --frames 60
build\bin\oe.exe exec MyGame scene.summary
build\bin\oe.exe exec MyGame entity.create "{\"name\":\"Box\",\"components\":{\"MeshRenderer\":{\"color\":\"#ff8800\"}}}" --save
build\bin\oe.exe import MyGame model.glb     :: 外部のモデル/テクスチャ/サウンドをプロジェクトにコピー
build\bin\oe.exe package MyGame             :: 配布用フォルダー dist\MyGame\ を作成 (MyGame.exe + game\)
build\bin\oe.exe package MyGame --web       :: Web 配布フォルダー dist\MyGame-web\ (index.html + wasm、どの静的ホスティングでも可)
build\bin\oe.exe serve dist\MyGame-web      :: Web ビルドをローカルで実行 (http://127.0.0.1:8080)
build\bin\oe.exe package MyGame --android   :: Android APK dist\MyGame-android\MyGame.apk (NDK 不要。SDK build-tools + Java が必要。--install でスマホにインストール)
build\bin\oe.exe api --markdown             :: コマンドリファレンスを出力
```

## エディター

ネットワークのロビー/RPC: `project.json` に
`"network":{"mode":"lockstep","transport":"tcp","port":7778}` を設定し、
継続して動作するツールセッションまたは Lua から `net.host`/`net.join` を呼びます。
`net.players`、`net.ready`、`net.stats`、`net.rpc`、`net.on` でロビーとメッセージを扱います。
設定なし/`none` は従来のシングルプレイヤーとローカル RPC です。
network.actions/axes を宣言し、全員が ready になったら net.start でロックステップを開始します。
input.player(id)、同期ずれレポート、実験的な履歴再実行ロールバックを利用できます。高速な
ネイティブロールバック（M5c）とサーバー複製（M6）を実装。NetSync/NetPlayer、差分、補間、所有者予測に対応。ブラウザーには対応 WebSocket サーバーと
再ビルド済みランタイムが必要です。[docs/NETWORK.md](docs/NETWORK.md) を参照。

`oe editor` は**エディター** (Dear ImGui のドッキング + ImGuizmo、エンジンと同じプロセスで GPU 描画) を開きます。コマンド API だけを使うため、エージェント (`oe mcp --connect 7777`) が人と同じセッションを同時に操作できます。ウィンドウがない環境 (ヘッドレスの Linux など) では `oe editor MyGame --screenshot shot.png` でエディター画面を画像にできます。詳細: [docs/EDITOR.md](docs/EDITOR.md)

- ドッキングパネル: ヒエラルキー・インスペクター・シーン・ゲーム・アセット・コンソール・スクリプト - 配置はプロジェクトごとに保存 (`.oe/editor.ini`)、表示 > レイアウトをリセット
- シーンビュー: 右ドラッグ + WASD/QE で飛行、中ボタンでパン、Alt + 左ドラッグでオービット、ホイールでズーム、クリックで選択、**移動/回転/スケールのギズモ** (Q/W/E/R、ローカル/ワールド、スナップ)、カメラ・ライトのアイコン、コライダー表示、2D ビュー、タイルの塗装 (Tiles パネル)、アセットをドラッグして配置
- ゲームビュー: クリックするとキーボードとマウスをゲームに渡します (マウスをロックするゲームには生のマウス移動量、Esc で解放)、アスペクト比の固定 (16:9 など)
- ヒエラルキー: 複数選択 (Ctrl/Shift)、ドラッグで親を変更、右クリックメニュー (名前を変更、複製、削除、子を作成、プレハブとして保存)
- インスペクター: リフレクションから自動生成、1 回のドラッグ = 元に戻す 1 段階、アセットのフィールドは選択リスト + ドラッグ＆ドロップ
- アセット: ダブルクリックでシーンを開く / スクリプトを編集 / プレハブを配置、エクスプローラーからファイルをウィンドウにドロップしてインポート
- スクリプト: Lua コードエディター - シンタックスハイライト、行番号、検索 / 置換 / すべて置換 (Ctrl+F)、入力中に構文エラーやグローバル変数のミスを表示 (行マーカー + 下線 + 問題一覧)、実行時エラーも表示、Ctrl+S で保存 → ホットリロード
- インスペクターの Script パラメーター: スクリプトが読む値を自動で見つけ、型に応じたフィールド (数値・チェックボックス・選択肢・ベクトル・色・シーン選択) で表示、既定値は薄く、リセットボタン、スクリプトが使わないキーは警告
- コンソール: ログフィルター + コマンド入力 (Tab で補完)
- AI が API で行った変更は通知とヒエラルキーの印でリアルタイムに反映、未保存の変更は閉じる時やシーン切り替え時に確認
- インターフェース言語: 日本語・English・한국어 (OS の言語から自動選択、表示 > 言語 または `--lang ja|en|ko`)、日本語・韓国語の入力と表示 (システムフォントを自動で統合、IME)、高 DPI 対応、インターフェースサイズの調整
- ショートカット: Ctrl+S 保存、Ctrl+Z/Y 元に戻す/やり直し、Ctrl+D 複製、Del 削除、F フォーカス、F2 名前を変更、Ctrl+P 再生/停止
- エージェント向けのエディタースクリーンショット: `oe editor MyGame --screenshot shot.png [--select Player] [--play --frames 60] [--lang ja]`


専用サーバー: `oe serve-game <project> --min-players 1` またはパッケージの `Game.exe --server`。ウィンドウ/GPU/音声デバイスを作成しません。TCP/UDP とネイティブ WebSocket サーバーに対応。エディターの Players で最大8人の Loopback テスト、Game Player で表示・入力先を選択、Network で遅延・統計を確認。コマンド: `net.spawn_local_peers`, `net.peer_call`, `net.local_peers`, `net.simulate`。使用方法とプラットフォーム検証範囲: [docs/NETWORK.md](docs/NETWORK.md)。

Network パネルは「表示 > ネットワーク」から開けます。ネットワーク未設定のプロジェクトにも設定案内を表示し、既存のエディターレイアウトに新しいタブを自動追加します。

## AI 連携

Lua `save.get/set/delete/flush` または `save.*` コマンドで進行状況を保存します。
ツールは既定でメモリを使用し、`--save-dir <dir>` で JSON スロットの永続保存を
有効にできます。詳細: [docs/SAVE.md](docs/SAVE.md)。

| 方法 | コマンド |
|---|---|
| MCP (Claude Code など) | `oe mcp <project> [--port 7777]` - すべてのコマンドが MCP ツール、スクリーンショットは画像で返ります。`--port` を指定すると人が同じセッションをエディターで見られます |
| 実行中のエディターに接続 | `oe mcp --connect 7777` |
| CLI / スクリプト | `oe exec`、`oe script` (JSON の入出力、失敗時は終了コード 1) |
| HTTP | `POST /api/call {"command": "...", "args": {...}}` |

このリポジトリの [.mcp.json](.mcp.json) は Claude Code で `samples/Hello` プロジェクトを MCP サーバーとして接続します (先にビルドが必要)。エージェント向けの作業ガイドは [CLAUDE.md](CLAUDE.md) / [AGENTS.md](AGENTS.md)、API 全体は [docs/API.md](docs/API.md)、構成は [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)、コードを変更するときの設計方針 (人間・AI エージェント共通) は [docs/DESIGN.md](docs/DESIGN.md) にあります。

## 構成

```
engine/core      Json, Math, Log, PNG, FileSystem
engine/scene     リフレクション、コンポーネント、Scene、システム
engine/script    Lua スクリプトホスト (ScriptHost)
engine/physics   物理 (Jolt ラッパー PhysicsWorld、Box2D ラッパー Physics2D)
engine/audio     オーディオミキサー、WAV、効果音ジェネレーター
engine/assets    アセット管理、glTF/画像の読み込み
engine/render    IRenderer、ソフトウェアラスタライザー、GPU レンダラー (sokol_gfx) + シェーダー、メッシュ、ゲーム UI
engine/api       コマンドレジストリ、HTTP サーバー、エディター用ルート、MCP サーバー
engine/app       Engine (シミュレーション、undo、ジョブキュー)、プロジェクトテンプレート
engine/editor    ネイティブエディター (Dear ImGui パネル、ギズモ、シーン/ゲームビュー、翻訳)
engine/platform  Platform.h + win32 (D3D11) / web (WebGL2) / android (GLES3) / null (EGL) の実装
tools/oe         CLI
tools/player     ゲームランタイム (Name.exe / Web の wasm / Android の .so) + Web ページのテンプレート
tools/shaders    シェーダー再生成スクリプト (sokol-shdc)
tests/           セルフテスト
third_party/lua  Lua 5.4.8 (MIT)
third_party/jolt Jolt Physics 5.6.0 (MIT)
third_party/box2d Box2D 3.1.1 (MIT)
third_party/stb, cgltf  画像デコーダー/エンコーダー、glTF デコーダー (PD/MIT, MIT)
third_party/sokol  sokol_gfx + sokol_imgui (zlib) - D3D11 / WebGL2 / GLES3 の抽象化
third_party/imgui, imguizmo, imguicolortextedit  Dear ImGui 1.92.9b docking、ImGuizmo、ImGuiColorTextEdit (MIT) - ネイティブエディター専用
templates/       `oe new` のプロジェクトテンプレート
samples/Hello    サンプルプロジェクト (テンプレートから生成)
samples/Showcase レンダリングサンプル (glTF のキツネキャラクター、テクスチャ、影、ポイントライト)
samples/Dungeon  2D 見下ろしアクション (Box2D、オートタイルのタイルセット、押せる木箱、魔法弾、スライム)
samples/Platformer 2D 横スクロールのプラットフォーマー (テキストのタイルマップ、スプライトアニメーション、敵、? ブロック、パララックス)
samples/FPS      一人称シューティングのテストゲーム (マウス視点、ヒットスキャンのピストル、リロード、動く標的、結果画面)
```

ネットワークサンプル: `samples/NetCoop`（lockstep 協力）、`samples/NetDuel`（rollback 対戦）、`samples/NetArena`（サーバー権限対戦）。エディターの Players 2、またはゲーム内 Host/Join/Ready/Start で開始。キーボード・ゲームパッド・タッチボタンに対応。起動・WebSocket・Android 検証: [docs/NETWORK_SAMPLES.md](docs/NETWORK_SAMPLES.md)。
