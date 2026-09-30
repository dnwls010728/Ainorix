# OwnEngine

[한국어](README.md) | [English](README.en.md) | **日本語**

AI が人と同じように扱い、検証できるように設計した C++17 ゲームエンジンです。人のためのエディターと AI のためのインターフェース (CLI・HTTP・MCP) が**同じコマンド API** を共有します。

![ネイティブエディター - Showcase サンプル](docs/images/native-editor.png)

| ゲーム画面 (`oe render samples/Showcase`) | |
|---|---|
| ![Showcase のレンダリング](docs/images/showcase.png) | glTF のキツネキャラクター (プレイヤー操作 + 追従カメラ)、手続き生成テクスチャ、影、ポイントライト。決定的なソフトウェアレンダラーの出力 (`oe render`) で、ゲームウィンドウ・Web・エディタービューは同じシーンを GPU レンダラーで描画します。 |

- 別途インストールする依存関係なし (MSVC + CMake のみ。Lua・Jolt Physics・sokol_gfx・stb・cgltf・Dear ImGui・ImGuizmo はソースを同梱)。Web ランタイムも `runtime/web/` にビルド済みで同梱 - Web 配布に Emscripten は不要
- Windows (ネイティブウィンドウ、`Name.exe` パッケージ)・Web (WebAssembly + WebGL2、`oe package --web`)・ヘッドレスに対応。Android / iOS / macOS / コンソールはプラットフォーム層を追加するだけで済むよう分離 - [docs/PLATFORMS.md](docs/PLATFORMS.md)
- 2 つのレンダラーが同じシーンを描画: **GPU レンダラー** (sokol_gfx - Windows は D3D11、Web は WebGL2、Linux は GLES3。4x MSAA、フィルタリングされた影、ミップマップ、シェーダーは `engine/render/shaders/Shaders.glsl` 1 つ) はゲームウィンドウ・Web・エディタービュー用、**ソフトウェアレンダラー** (マルチスレッド、決定的) はスクリーンショットのハッシュ・テスト・ピッキング用
- レンダリング: glTF モデル、PNG/JPEG テクスチャ、スムーズ/フラットシェーディング、ポイントライト、影、平行投影/追従カメラ、デバッグ描画 - [docs/RENDERING.md](docs/RENDERING.md)
- マテリアル / PBR / 半透明: metallic-roughness (GGX) シェーディング、法線・AO・発光マップ、半透明 (奥から手前へソート)・マスク・両面、glTF マテリアルの完全読み込み、`.mat.json` マテリアルファイル (`material.create` / `material.set`、ホットリロード) - [docs/RENDERING.md](docs/RENDERING.md)
- Jolt による 3D 物理 (剛体、トリガー、キャラクターコントローラー、決定的シミュレーション) - [docs/PHYSICS.md](docs/PHYSICS.md)
- ゲーム UI: TrueType フォント (あらゆる言語、フォントファイルを追加)、アンカー・ストレッチ・親子配置、レイアウト (縦/横/グリッド、内容に合わせたサイズ)、リッチテキスト・折り返し・アウトライン・影、角丸・枠付きパネル、ボタンの状態 (ホバー/押下/無効)、画像 (9-slice、フィルバー)、スライダー/プログレスバー、クリッピング、キャンバススケール - [docs/UI.md](docs/UI.md)
- 2D ゲーム: スプライト・スプライトシートアニメーション (ピクセルアート、透明の切り抜き)、テキストで書くタイルマップ (衝突を自動生成)、XY 平面の物理、範囲付き追従カメラ、エディターの 2D ビュー - [docs/2D.md](docs/2D.md)
- ゲームの構成要素: プレハブ、シーン切り替え + ゲームデータ、メッセージ/タイマー、ゲーム内 UI、オーディオ (決定的ミキサー、効果音の生成) - [docs/GAMEPLAY.md](docs/GAMEPLAY.md)
- Lua 5.4 スクリプト (サンドボックス、ホットリロード、エラーにファイル:行を表示) - [docs/SCRIPTING.md](docs/SCRIPTING.md)
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
build\bin\oe.exe api --markdown             :: コマンドリファレンスを出力
```

## エディター

`oe editor` は**エディター** (Dear ImGui のドッキング + ImGuizmo、エンジンと同じプロセスで GPU 描画) を開きます。コマンド API だけを使うため、エージェント (`oe mcp --connect 7777`) が人と同じセッションを同時に操作できます。ウィンドウがない環境 (ヘッドレスの Linux など) では `oe editor MyGame --screenshot shot.png` でエディター画面を画像にできます。詳細: [docs/EDITOR.md](docs/EDITOR.md)

- ドッキングパネル: ヒエラルキー・インスペクター・シーン・ゲーム・アセット・コンソール・スクリプト - 配置はプロジェクトごとに保存 (`.oe/editor.ini`)、表示 > レイアウトをリセット
- シーンビュー: 右ドラッグ + WASD/QE で飛行、中ボタンでパン、Alt + 左ドラッグでオービット、ホイールでズーム、クリックで選択、**移動/回転/スケールのギズモ** (Q/W/E/R、ローカル/ワールド、スナップ)、カメラ・ライトのアイコン、コライダー表示、2D ビュー、アセットをドラッグして配置
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


## AI 連携

| 方法 | コマンド |
|---|---|
| MCP (Claude Code など) | `oe mcp <project> [--port 7777]` - すべてのコマンドが MCP ツール、スクリーンショットは画像で返ります。`--port` を指定すると人が同じセッションをエディターで見られます |
| 実行中のエディターに接続 | `oe mcp --connect 7777` |
| CLI / スクリプト | `oe exec`、`oe script` (JSON の入出力、失敗時は終了コード 1) |
| HTTP | `POST /api/call {"command": "...", "args": {...}}` |

このリポジトリの [.mcp.json](.mcp.json) は Claude Code で `samples/Hello` プロジェクトを MCP サーバーとして接続します (先にビルドが必要)。エージェント向けの作業ガイドは [CLAUDE.md](CLAUDE.md) / [AGENTS.md](AGENTS.md)、API 全体は [docs/API.md](docs/API.md)、構成は [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) にあります。

## 構成

```
engine/core      Json, Math, Log, PNG, FileSystem
engine/scene     リフレクション、コンポーネント、Scene、システム
engine/script    Lua スクリプトホスト (ScriptHost)
engine/physics   物理 (Jolt ラッパー、PhysicsWorld)
engine/audio     オーディオミキサー、WAV、効果音ジェネレーター
engine/assets    アセット管理、glTF/画像の読み込み
engine/render    IRenderer、ソフトウェアラスタライザー、GPU レンダラー (sokol_gfx) + シェーダー、メッシュ、ゲーム UI
engine/api       コマンドレジストリ、HTTP サーバー、エディター用ルート、MCP サーバー
engine/app       Engine (シミュレーション、undo、ジョブキュー)、プロジェクトテンプレート
engine/editor    ネイティブエディター (Dear ImGui パネル、ギズモ、シーン/ゲームビュー、翻訳)
engine/platform  Platform.h + win32 (D3D11) / web (WebGL2) / null (EGL) の実装
tools/oe         CLI
tools/player     ゲームランタイム (Name.exe / Web の wasm) + Web ページのテンプレート
tools/shaders    シェーダー再生成スクリプト (sokol-shdc)
tests/           セルフテスト
third_party/lua  Lua 5.4.8 (MIT)
third_party/jolt Jolt Physics 5.6.0 (MIT)
third_party/stb, cgltf  画像デコーダー/エンコーダー、glTF デコーダー (PD/MIT, MIT)
third_party/sokol  sokol_gfx + sokol_imgui (zlib) - D3D11 / WebGL2 / GLES3 の抽象化
third_party/imgui, imguizmo, imguicolortextedit  Dear ImGui 1.92.9b docking、ImGuizmo、ImGuiColorTextEdit (MIT) - ネイティブエディター専用
templates/       `oe new` のプロジェクトテンプレート
samples/Hello    サンプルプロジェクト (テンプレートから生成)
samples/Showcase レンダリングサンプル (glTF のキツネキャラクター、テクスチャ、影、ポイントライト)
samples/Platformer 2D 横スクロールのプラットフォーマー (テキストのタイルマップ、スプライトアニメーション、敵、? ブロック、パララックス)
samples/FPS      一人称シューティングのテストゲーム (マウス視点、ヒットスキャンのピストル、リロード、動く標的、結果画面)
```
