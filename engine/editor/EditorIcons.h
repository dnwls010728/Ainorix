#pragma once
#include <string>

// Line icons from Lucide (third_party/lucide, ISC license), merged into the editor's UI font
// over the private-use range, so they can be written inside any label: Format("%s  %s", kIconFolder, ...).
// To add one: find its name and code in lucide-static's font/info.json (the version in
// third_party/lucide/README.md) and add the UTF-8 bytes here.

namespace oe {

constexpr unsigned kIconFirst = 0xE000, kIconLast = 0xE9FF;  // glyph range merged from the icon font

constexpr const char* kIconPlay                  = "\xee\x85\x80";  // play U+E140
constexpr const char* kIconPause                 = "\xee\x84\xb2";  // pause U+E132
constexpr const char* kIconSquare                = "\xee\x85\xab";  // square U+E16B
constexpr const char* kIconStepForward           = "\xee\x8f\xae";  // step-forward U+E3EE
constexpr const char* kIconMousePointer2         = "\xee\x87\x83";  // mouse-pointer-2 U+E1C3
constexpr const char* kIconMove                  = "\xee\x84\xa5";  // move U+E125
constexpr const char* kIconRotate3d              = "\xee\x8b\xaa";  // rotate-3d U+E2EA
constexpr const char* kIconScale3d               = "\xee\x8b\xab";  // scale-3d U+E2EB
constexpr const char* kIconVideo                 = "\xee\x86\xa5";  // video U+E1A5
constexpr const char* kIconSun                   = "\xee\x85\xbc";  // sun U+E17C
constexpr const char* kIconLightbulb             = "\xee\x87\x82";  // lightbulb U+E1C2
constexpr const char* kIconPersonStanding        = "\xee\x88\x9e";  // person-standing U+E21E
constexpr const char* kIconAtom                  = "\xee\x8f\x9b";  // atom U+E3DB
constexpr const char* kIconBox                   = "\xee\x81\xa5";  // box U+E065
constexpr const char* kIconImage                 = "\xee\x83\xb9";  // image U+E0F9
constexpr const char* kIconGrid3x3               = "\xee\x83\xac";  // grid-3x3 U+E0EC
constexpr const char* kIconType                  = "\xee\x86\x98";  // type U+E198
constexpr const char* kIconLayoutTemplate        = "\xee\x88\x87";  // layout-template U+E207
constexpr const char* kIconVolume2               = "\xee\x86\xab";  // volume-2 U+E1AB
constexpr const char* kIconSparkles              = "\xee\x90\x97";  // sparkles U+E417
constexpr const char* kIconBraces                = "\xee\x8d\xae";  // braces U+E36E
constexpr const char* kIconCircleDashed          = "\xee\x92\xb5";  // circle-dashed U+E4B5
constexpr const char* kIconEye                   = "\xee\x82\xbe";  // eye U+E0BE
constexpr const char* kIconEyeOff                = "\xee\x82\xbf";  // eye-off U+E0BF
constexpr const char* kIconListTree              = "\xee\x90\x8d";  // list-tree U+E40D
constexpr const char* kIconSlidersHorizontal     = "\xee\x8a\x9a";  // sliders-horizontal U+E29A
constexpr const char* kIconFolder                = "\xee\x83\x9c";  // folder U+E0DC
constexpr const char* kIconTerminal              = "\xee\x86\x85";  // terminal U+E185
constexpr const char* kIconGamepad2              = "\xee\x83\xa2";  // gamepad-2 U+E0E2
constexpr const char* kIconFileCode              = "\xee\x83\x87";  // file-code U+E0C7
constexpr const char* kIconUsers                 = "\xee\x86\xa4";  // users U+E1A4
constexpr const char* kIconBot                   = "\xee\x86\xbb";  // bot U+E1BB
constexpr const char* kIconSend                  = "\xee\x85\x96";  // send U+E156
constexpr const char* kIconMapPin                = "\xee\x84\x95";  // map-pin U+E115
constexpr const char* kIconMessagesSquare        = "\xee\x90\x92";  // messages-square U+E412
constexpr const char* kIconHistory               = "\xee\x87\xb5";  // history U+E1F5
constexpr const char* kIconNetwork               = "\xee\x84\xa9";  // network U+E129
constexpr const char* kIconWorkflow              = "\xee\x90\xaa";  // workflow U+E42A
constexpr const char* kIconMusic                 = "\xee\x84\xa6";  // music U+E126
constexpr const char* kIconFile                  = "\xee\x83\x84";  // file U+E0C4
constexpr const char* kIconPlus                  = "\xee\x85\x81";  // plus U+E141
constexpr const char* kIconSearch                = "\xee\x85\x95";  // search U+E155
constexpr const char* kIconMagnet                = "\xee\x8a\xb5";  // magnet U+E2B5
constexpr const char* kIconGlobe                 = "\xee\x83\xab";  // globe U+E0EB
constexpr const char* kIconSave                  = "\xee\x85\x91";  // save U+E151
constexpr const char* kIconLayers                = "\xee\x84\x84";  // layers U+E104
constexpr const char* kIconX                     = "\xee\x86\xb2";  // x U+E1B2
constexpr const char* kIconChevronDown           = "\xee\x81\xb1";  // chevron-down U+E071
constexpr const char* kIconChevronRight          = "\xee\x81\xb3";  // chevron-right U+E073
constexpr const char* kIconPackage               = "\xee\x84\xad";  // package U+E12D
constexpr const char* kIconPalette               = "\xee\x87\x9d";  // palette U+E1DD
constexpr const char* kIconFileJson              = "\xee\x8d\xaf";  // file-json U+E36F
constexpr const char* kIconFilm                  = "\xee\x83\x94";  // film U+E0D4
constexpr const char* kIconAperture              = "\xee\x81\x84";  // aperture U+E044
constexpr const char* kIconSquareDashed          = "\xee\x87\x8b";  // square-dashed U+E1CB
constexpr const char* kIconMove3d                = "\xee\x8b\xa5";  // move-3d U+E2E5
constexpr const char* kIconTrash2                = "\xee\x86\x8e";  // trash-2 U+E18E
constexpr const char* kIconTriangleAlert         = "\xee\x86\x93";  // triangle-alert U+E193
constexpr const char* kIconCircleX               = "\xee\x82\x88";  // circle-x U+E088
constexpr const char* kIconInfo                  = "\xee\x83\xbe";  // info U+E0FE
constexpr const char* kIconBug                   = "\xee\x88\x8c";  // bug U+E20C
constexpr const char* kIconArrowDownToLine       = "\xee\x91\x9a";  // arrow-down-to-line U+E45A
constexpr const char* kIconLayoutGrid            = "\xee\x84\x85";  // layout-grid U+E105
constexpr const char* kIconList                  = "\xee\x84\x8c";  // list U+E10C
constexpr const char* kIconRefreshCw             = "\xee\x85\x89";  // refresh-cw U+E149
constexpr const char* kIconCircleHelp            = "\xee\x82\x82";  // circle-help U+E082
constexpr const char* kIconCpu                   = "\xee\x82\xad";  // cpu U+E0AD
constexpr const char* kIconGauge                 = "\xee\x86\xbf";  // gauge U+E1BF
constexpr const char* kIconPaperclip             = "\xee\x84\xb1";  // paperclip U+E131
constexpr const char* kIconSettings              = "\xee\x85\x98";  // settings U+E158
constexpr const char* kIconCircleCheck           = "\xee\x88\xa6";  // circle-check U+E226
constexpr const char* kIconLogIn                 = "\xee\x84\x91";  // log-in U+E111
constexpr const char* kIconPlug                  = "\xee\x8e\x83";  // plug U+E383

// A window or popup title with an icon in front: WithIcon(kIconFolder, TrId("Assets")). The `###` id
// part of TrId stays the same, so docked layouts and settings keep working.
inline std::string WithIcon(const char* icon, const std::string& title) { return std::string(icon) + "  " + title; }

}  // namespace oe
