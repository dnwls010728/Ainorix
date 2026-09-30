// Native editor translations. Add a row when you add a UI string; the
// EditorTranslations test fails for strings shown without one.
#include "editor/EditorText.h"

#include <cctype>
#include <mutex>
#include <unordered_map>

namespace oe {

namespace {

struct Entry {
    const char* en;
    const char* ko;
    const char* ja;
};

const Entry kCatalog[] = {
    // ----- Panels
    {"Hierarchy", "계층", "ヒエラルキー"},
    {"Inspector", "인스펙터", "インスペクター"},
    {"Scene", "씬", "シーン"},
    {"Game", "게임", "ゲーム"},
    {"Assets", "에셋", "アセット"},
    {"Console", "콘솔", "コンソール"},
    {"Scripts", "스크립트", "スクリプト"},

    // ----- Main menu
    {"File", "파일", "ファイル"},
    {"New Scene", "새 씬", "新規シーン"},
    {"Open Scene", "씬 열기", "シーンを開く"},
    {"Save Scene", "씬 저장", "シーンを保存"},
    {"Save Scene As...", "다른 이름으로 씬 저장...", "名前を付けてシーンを保存..."},
    {"Import: drop files on the window", "가져오기: 파일을 창에 끌어다 놓기", "インポート: ファイルをウィンドウにドロップ"},
    {"Exit", "종료", "終了"},
    {"Edit", "편집", "編集"},
    {"Undo", "실행 취소", "元に戻す"},
    {"Redo", "다시 실행", "やり直し"},
    {"Duplicate", "복제", "複製"},
    {"Delete", "삭제", "削除"},
    {"Rename", "이름 변경", "名前を変更"},
    {"Save as Prefab...", "프리팹으로 저장...", "プレハブとして保存..."},
    {"Frame Selected", "선택 항목에 초점", "選択範囲にフォーカス"},
    {"Select None", "선택 해제", "選択解除"},
    {"Create", "만들기", "作成"},
    {"View", "보기", "表示"},
    {"Grid", "그리드", "グリッド"},
    {"Colliders", "콜라이더", "コライダー"},
    {"Entity Icons", "엔티티 아이콘", "エンティティアイコン"},
    {"2D Scene View", "2D 씬 뷰", "2D シーンビュー"},
    {"Interface Size", "인터페이스 크기", "インターフェースサイズ"},
    {"Language", "언어", "言語"},
    {"Reset Layout", "레이아웃 초기화", "レイアウトをリセット"},
    {"ImGui Metrics", "ImGui 지표", "ImGui メトリクス"},
    {"Play", "플레이", "再生"},
    {"Stop", "정지", "停止"},
    {"Pause", "일시정지", "一時停止"},
    {"Resume", "계속", "再開"},
    {"Step One Frame", "한 프레임 진행", "1 フレーム進める"},
    {"Help", "도움말", "ヘルプ"},
    {"Scene view", "씬 뷰", "シーンビュー"},
    {"Right mouse: look around, WASD/QE fly (wheel = speed)", "오른쪽 버튼: 둘러보기, WASD/QE 비행 (휠 = 속도)", "右ボタン: 見回す、WASD/QE で飛行 (ホイール = 速度)"},
    {"Middle mouse: pan    Alt + left mouse: orbit    Wheel: zoom", "가운데 버튼: 이동    Alt + 왼쪽 버튼: 궤도 회전    휠: 확대/축소",
     "中ボタン: パン    Alt + 左ボタン: オービット    ホイール: ズーム"},
    {"Q select, W move, E rotate, R scale, X local/world, F frame", "Q 선택, W 이동, E 회전, R 크기, X 로컬/월드, F 초점",
     "Q 選択、W 移動、E 回転、R スケール、X ローカル/ワールド、F フォーカス"},
    {"Ctrl while dragging a gizmo: snap", "기즈모를 드래그하며 Ctrl: 스냅", "ギズモのドラッグ中に Ctrl: スナップ"},
    {"Game view", "게임 뷰", "ゲームビュー"},
    {"Click it to give the game keyboard and mouse; Esc frees a locked mouse", "클릭하면 키보드와 마우스가 게임으로 전달, Esc로 잠긴 마우스 해제",
     "クリックするとキーボードとマウスをゲームに渡します。Esc でロックされたマウスを解放"},
    {"Everywhere", "공통", "共通"},
    {"Ctrl+S save, Ctrl+Z/Y undo/redo, Ctrl+D duplicate, Del delete, Ctrl+P play/stop",
     "Ctrl+S 저장, Ctrl+Z/Y 실행 취소/다시 실행, Ctrl+D 복제, Del 삭제, Ctrl+P 플레이/정지",
     "Ctrl+S 保存、Ctrl+Z/Y 元に戻す/やり直し、Ctrl+D 複製、Del 削除、Ctrl+P 再生/停止"},
    {"Drop files on the window to import them into the project", "파일을 창에 끌어다 놓으면 프로젝트로 가져옵니다",
     "ファイルをウィンドウにドロップするとプロジェクトにインポートします"},
    {"OwnEngine %s - agents can attach with: oe mcp --connect <port>", "OwnEngine %s - 에이전트 연결: oe mcp --connect <포트>",
     "OwnEngine %s - エージェントの接続: oe mcp --connect <ポート>"},

    // ----- Toolbar and status bar
    {"Select (Q)", "선택 (Q)", "選択 (Q)"},
    {"Move (W)", "이동 (W)", "移動 (W)"},
    {"Rotate (E)", "회전 (E)", "回転 (E)"},
    {"Scale (R)", "크기 (R)", "スケール (R)"},
    {"Local", "로컬", "ローカル"},
    {"World", "월드", "ワールド"},
    {"Gizmo orientation (X)", "기즈모 방향 (X)", "ギズモの向き (X)"},
    {"Snap", "스냅", "スナップ"},
    {"Snap gizmo moves (hold Ctrl to toggle while dragging)", "기즈모 조작을 스냅 (드래그 중 Ctrl로 전환)", "ギズモ操作をスナップ (ドラッグ中に Ctrl で切り替え)"},
    {"Move snap", "이동 스냅", "移動スナップ"},
    {"Rotate snap", "회전 스냅", "回転スナップ"},
    {"Stop (Ctrl+P) - restores the scene", "정지 (Ctrl+P) - 씬 복원", "停止 (Ctrl+P) - シーンを復元"},
    {"Play (Ctrl+P)", "플레이 (Ctrl+P)", "再生 (Ctrl+P)"},
    {"Step one frame (1/60 s)", "한 프레임 진행 (1/60초)", "1 フレーム進める (1/60 秒)"},
    {"(unsaved scene)", "(저장 안 된 씬)", "(未保存のシーン)"},
    {"Save", "저장", "保存"},
    {"PLAYING", "플레이 중", "再生中"},
    {"PAUSED", "일시정지됨", "一時停止中"},
    {"frame %llu", "프레임 %llu", "フレーム %llu"},
    {"|  %d entities  |  %d fps  |  %s", "|  엔티티 %d개  |  %d fps  |  %s", "|  エンティティ %d  |  %d fps  |  %s"},
    {"%d errors", "오류 %d개", "エラー %d"},
    {"%d warnings", "경고 %d개", "警告 %d"},

    // ----- Prompts and notices
    {"Save changes?", "변경 사항을 저장할까요?", "変更を保存しますか?"},
    {"Save the changes to \"%s\" first?", "\"%s\"의 변경 사항을 먼저 저장할까요?", "\"%s\" の変更を先に保存しますか?"},
    {"Don't Save", "저장 안 함", "保存しない"},
    {"Cancel", "취소", "キャンセル"},
    {"Save Scene As", "다른 이름으로 씬 저장", "名前を付けてシーンを保存"},
    {"Path in the project (*.scene.json):", "프로젝트 안의 경로 (*.scene.json):", "プロジェクト内のパス (*.scene.json):"},
    {"Save as Prefab", "프리팹으로 저장", "プレハブとして保存"},
    {"Prefab file (*.prefab.json), with the selected entity and its children:", "프리팹 파일 (*.prefab.json), 선택한 엔티티와 자식 포함:",
     "プレハブファイル (*.prefab.json)。選択したエンティティと子を含みます:"},
    {"Stop the game before saving (the scene is restored on stop).", "저장하기 전에 게임을 정지하세요 (정지하면 씬이 복원됩니다).",
     "保存する前にゲームを停止してください (停止するとシーンが復元されます)。"},
    {"Saved %s", "%s 저장됨", "%s を保存しました"},
    {"Created %s", "%s 생성됨", "%s を作成しました"},
    {"Opened %s", "%s 열림", "%s を開きました"},
    {"Import failed: %s", "가져오기 실패: %s", "インポートに失敗しました: %s"},
    {"Imported %s", "%s 가져옴", "%s をインポートしました"},
    {"Imported %d files", "파일 %d개를 가져왔습니다", "%d 個のファイルをインポートしました"},
    {"%s: invalid JSON - %s", "%s: 잘못된 JSON - %s", "%s: 無効な JSON - %s"},
    {"Drop a material on an entity with a MeshRenderer", "머티리얼은 MeshRenderer가 있는 엔티티에 놓으세요",
     "マテリアルは MeshRenderer を持つエンティティにドロップしてください"},
    {"Drop a script on an entity", "스크립트는 엔티티에 놓으세요", "スクリプトはエンティティにドロップしてください"},
    {"Nothing to place for %s", "%s은(는) 배치할 수 없습니다", "%s は配置できません"},

    // ----- Hierarchy
    {"Search", "검색", "検索"},
    {"Create an entity", "엔티티 만들기", "エンティティを作成"},
    {"Create Empty Child", "빈 자식 만들기", "空の子を作成"},
    {"Move to Root", "최상위로 이동", "ルートに移動"},
    {"Frame in Scene View", "씬 뷰에서 초점", "シーンビューでフォーカス"},
    {"Changed through the API (agent) just now", "방금 API(에이전트)로 변경됨", "API (エージェント) によって変更されました"},

    // ----- Inspector
    {"(none)", "(없음)", "(なし)"},
    {"No %s files in the project yet", "프로젝트에 %s 파일이 아직 없습니다", "プロジェクトにまだ %s ファイルがありません"},
    {"Pick a %s from the project (or drag one from Assets)", "프로젝트에서 %s 선택 (또는 에셋에서 끌어오기)", "プロジェクトから %s を選択 (またはアセットからドラッグ)"},
    {"Select an entity in the Hierarchy or the Scene view.", "계층이나 씬 뷰에서 엔티티를 선택하세요.", "ヒエラルキーまたはシーンビューでエンティティを選択してください。"},
    {"%d entities selected - showing the first", "엔티티 %d개 선택됨 - 첫 번째를 표시", "%d 個のエンティティを選択中 - 最初のものを表示"},
    {"Remove Component", "컴포넌트 제거", "コンポーネントを削除"},
    {"Reset to Defaults", "기본값으로 초기화", "デフォルトに戻す"},
    {"Copy as JSON", "JSON으로 복사", "JSON としてコピー"},
    {"Remove %s", "%s 제거", "%s を削除"},
    {"Edit Script", "스크립트 편집", "スクリプトを編集"},
    {"Unknown component type", "알 수 없는 컴포넌트 타입", "不明なコンポーネントタイプ"},
    {"Add Component", "컴포넌트 추가", "コンポーネントを追加"},
    {"Search components", "컴포넌트 검색", "コンポーネントを検索"},

    // ----- Assets
    {"Refresh", "새로 고침", "更新"},
    {"Drop files on the window to import. Drag assets into the Scene, Hierarchy or Inspector.",
     "파일을 창에 놓으면 가져옵니다. 에셋은 씬, 계층, 인스펙터로 끌어다 놓을 수 있습니다.",
     "ファイルをウィンドウにドロップするとインポートします。アセットはシーン、ヒエラルキー、インスペクターにドラッグできます。"},
    {"Scenes", "씬", "シーン"},
    {"Prefabs", "프리팹", "プレハブ"},
    {"Models", "모델", "モデル"},
    {"Textures", "텍스처", "テクスチャ"},
    {"Materials", "머티리얼", "マテリアル"},
    {"Sounds", "사운드", "サウンド"},
    {"Fonts", "폰트", "フォント"},
    {"Other", "기타", "その他"},
    {"Double-click to open", "더블클릭하여 열기", "ダブルクリックで開く"},
    {"Double-click to edit", "더블클릭하여 편집", "ダブルクリックで編集"},
    {"Double-click to instantiate", "더블클릭하여 배치", "ダブルクリックでインスタンス化"},
    {"Open", "열기", "開く"},
    {"Instantiate", "배치", "インスタンス化"},
    {"Copy Path", "경로 복사", "パスをコピー"},
    {"Info", "정보", "情報"},
    {"The project has no assets yet.", "프로젝트에 아직 에셋이 없습니다.", "プロジェクトにまだアセットがありません。"},

    // ----- Console
    {"Debug", "디버그", "デバッグ"},
    {"Warnings", "경고", "警告"},
    {"Errors", "오류", "エラー"},
    {"Filter", "필터", "フィルター"},
    {"Clear", "지우기", "クリア"},
    {"Auto-scroll", "자동 스크롤", "自動スクロール"},
    {"Copy", "복사", "コピー"},
    {"command.name {\"json\": \"args\"}   (Tab completes, Up/Down history)", "command.name {\"json\": \"args\"}   (Tab 자동완성, 위/아래 이전 명령)",
     "command.name {\"json\": \"args\"}   (Tab で補完、上/下 で履歴)"},

    // ----- Scripts
    {"Double-click a script in Assets (or use \"Edit Script\" on a Script component) to edit it here.",
     "에셋에서 스크립트를 더블클릭하거나 Script 컴포넌트의 \"스크립트 편집\"을 누르면 여기서 편집합니다.",
     "アセットでスクリプトをダブルクリックするか、Script コンポーネントの「スクリプトを編集」でここで編集できます。"},
    {"Ctrl+S saves the script; it hot-reloads, also while the game runs.", "Ctrl+S로 저장하면 게임 실행 중에도 바로 다시 로드됩니다.",
     "Ctrl+S で保存すると、ゲーム実行中でもホットリロードされます。"},
    {"Revert", "되돌리기", "元に戻す"},
    {"  (modified)", "  (수정됨)", "  (変更あり)"},

    // ----- Scene and Game views
    {"Icons", "아이콘", "アイコン"},
    {"Ground grid", "바닥 그리드", "地面のグリッド"},
    {"Collider wireframes: green solid, yellow trigger, cyan character", "콜라이더 와이어프레임: 초록 고체, 노랑 트리거, 청록 캐릭터",
     "コライダーのワイヤーフレーム: 緑 = ソリッド、黄 = トリガー、シアン = キャラクター"},
    {"Markers for cameras, lights and sounds", "카메라, 조명, 사운드 표시", "カメラ、ライト、サウンドのマーカー"},
    {"Look along -Z (2D games); drag with right/middle mouse to pan", "-Z 방향으로 보기 (2D 게임), 오른쪽/가운데 버튼 드래그로 이동",
     "-Z 方向を見る (2D ゲーム)。右/中ボタンのドラッグでパン"},
    {"Fly: WASD / QE   Shift: faster   Wheel: speed %.1f m/s", "비행: WASD / QE   Shift: 빠르게   휠: 속도 %.1f m/s",
     "飛行: WASD / QE   Shift: 高速   ホイール: 速度 %.1f m/s"},
    {"Playing - edits are undone on Stop", "플레이 중 - 정지하면 편집 내용이 되돌려집니다", "再生中 - 停止すると編集は元に戻ります"},
    {"Paused - edits are undone on Stop", "일시정지 - 정지하면 편집 내용이 되돌려집니다", "一時停止中 - 停止すると編集は元に戻ります"},
    {"Free aspect", "자유 비율", "フリーアスペクト"},
    {"Game view aspect ratio", "게임 뷰 화면비", "ゲームビューのアスペクト比"},
    {"Press Play (Ctrl+P) to run the game here", "플레이(Ctrl+P)를 누르면 여기서 게임이 실행됩니다", "再生 (Ctrl+P) を押すとここでゲームが動きます"},
    {"Game has the mouse - Esc releases it", "게임이 마우스를 사용 중 - Esc로 해제", "ゲームがマウスを使用中 - Esc で解放"},
    {"Game has keyboard and mouse - click outside to give them back", "게임이 키보드와 마우스를 사용 중 - 바깥을 클릭하면 돌려받습니다",
     "ゲームがキーボードとマウスを使用中 - 外側をクリックすると戻ります"},
    {"Click the view to play", "뷰를 클릭하면 조작할 수 있습니다", "ビューをクリックして操作"},
    {"No active camera: add a Camera component (Create > Rendering > Camera)", "활성 카메라가 없습니다: Camera 컴포넌트를 추가하세요 (만들기 > 렌더링 > 카메라)",
     "アクティブなカメラがありません: Camera コンポーネントを追加してください (作成 > レンダリング > カメラ)"},

    // ----- Create menu presets (entity names stay English: scripts refer to them)
    {"Basic", "기본", "基本"},
    {"Rendering", "렌더링", "レンダリング"},
    {"Physics", "물리", "物理"},
    {"Empty", "빈 엔티티", "空のエンティティ"},
    {"Cube", "큐브", "キューブ"},
    {"Sphere", "구", "球"},
    {"Plane", "평면", "平面"},
    {"Pyramid", "피라미드", "ピラミッド"},
    {"Camera", "카메라", "カメラ"},
    {"Directional Light", "방향 조명", "ディレクショナルライト"},
    {"Point Light", "점 조명", "ポイントライト"},
    {"Physics Crate", "물리 상자", "物理ボックス"},
    {"Physics Ball", "물리 공", "物理ボール"},
    {"Static Wall", "고정 벽", "静的な壁"},
    {"Sprite", "스프라이트", "スプライト"},
    {"Tilemap", "타일맵", "タイルマップ"},
    {"2D Camera", "2D 카메라", "2D カメラ"},
    {"UI Text", "UI 텍스트", "UI テキスト"},
    {"UI Button", "UI 버튼", "UI ボタン"},
    {"UI Panel", "UI 패널", "UI パネル"},
    {"UI Image", "UI 이미지", "UI 画像"},
    {"UI Slider", "UI 슬라이더", "UI スライダー"},
    {"UI Progress Bar", "UI 진행 바", "UI プログレスバー"},
    {"UI Menu (vertical layout)", "UI 메뉴 (세로 레이아웃)", "UI メニュー (縦レイアウト)"},
};

EditorLanguage g_language = EditorLanguage::English;
std::mutex g_missingMutex;
std::set<std::string> g_missing;

const std::unordered_map<std::string, const Entry*>& Index() {
    static const std::unordered_map<std::string, const Entry*> index = [] {
        std::unordered_map<std::string, const Entry*> m;
        for (const Entry& e : kCatalog) m[e.en] = &e;
        return m;
    }();
    return index;
}

// printf conversions in order ("%s", "%d", "%.1f", ...), "%%" skipped.
std::string Conversions(const char* s) {
    std::string out;
    for (const char* p = s; *p; ++p) {
        if (*p != '%') continue;
        if (p[1] == '%') {
            ++p;
            continue;
        }
        const char* q = p + 1;
        while (*q && !std::isalpha(static_cast<unsigned char>(*q))) ++q;
        while (*q == 'l' || *q == 'h') ++q;
        if (*q) out += *q;
        out += ',';
        p = q;
    }
    return out;
}

}  // namespace

void SetEditorLanguage(EditorLanguage language) { g_language = language; }
EditorLanguage GetEditorLanguage() { return g_language; }

EditorLanguage ParseEditorLanguage(const std::string& code) {
    std::string c;
    for (char ch : code.substr(0, 2)) c += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (c == "ko") return EditorLanguage::Korean;
    if (c == "ja") return EditorLanguage::Japanese;
    return EditorLanguage::English;
}

const char* EditorLanguageCode(EditorLanguage language) {
    switch (language) {
        case EditorLanguage::Korean: return "ko";
        case EditorLanguage::Japanese: return "ja";
        default: return "en";
    }
}

const char* EditorLanguageName(EditorLanguage language) {
    switch (language) {
        case EditorLanguage::Korean: return "한국어";
        case EditorLanguage::Japanese: return "日本語";
        default: return "English";
    }
}

const char* Tr(const char* english) {
    if (g_language == EditorLanguage::English) return english;
    auto it = Index().find(english);
    if (it == Index().end()) {
        std::lock_guard<std::mutex> lock(g_missingMutex);
        g_missing.insert(english);
        return english;
    }
    const char* t = g_language == EditorLanguage::Korean ? it->second->ko : it->second->ja;
    return t && *t ? t : english;
}

std::string TrId(const char* english) { return std::string(Tr(english)) + "###" + english; }

std::set<std::string> EditorCatalogProblems() {
    std::set<std::string> problems;
    std::set<std::string> seen;
    for (const Entry& e : kCatalog) {
        if (!seen.insert(e.en).second) problems.insert(std::string("duplicate: ") + e.en);
        if (!e.ko || !*e.ko) problems.insert(std::string("no Korean: ") + e.en);
        if (!e.ja || !*e.ja) problems.insert(std::string("no Japanese: ") + e.en);
        std::string args = Conversions(e.en);
        if (e.ko && Conversions(e.ko) != args) problems.insert(std::string("Korean arguments differ: ") + e.en);
        if (e.ja && Conversions(e.ja) != args) problems.insert(std::string("Japanese arguments differ: ") + e.en);
    }
    return problems;
}

bool EditorCatalogHas(const std::string& english) { return Index().count(english) > 0; }

std::set<std::string> EditorMissingTranslations() {
    std::lock_guard<std::mutex> lock(g_missingMutex);
    return g_missing;
}

}  // namespace oe
