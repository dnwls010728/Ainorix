#pragma once
#include <set>
#include <string>

namespace oe {

// Interface language of the native editor. Strings are written in English in
// the code and looked up in the catalog in EditorText.cpp; missing entries
// fall back to English. API names (commands, component types and fields,
// entity names) are never translated.
enum class EditorLanguage { English, Korean, Japanese };

void SetEditorLanguage(EditorLanguage language);
EditorLanguage GetEditorLanguage();
// "ko", "ko-KR", "ja_JP.UTF-8", ... -> language; anything else -> English.
EditorLanguage ParseEditorLanguage(const std::string& code);
const char* EditorLanguageCode(EditorLanguage language);  // "en", "ko", "ja"
const char* EditorLanguageName(EditorLanguage language);  // in its own language: "English", "한국어", "日本語"

// Translation of an English UI string (format strings keep their % arguments).
const char* Tr(const char* english);
// Window / popup name with a language independent ID: "번역###English".
std::string TrId(const char* english);

// Catalog checks for tests: entries with a missing translation or different
// printf arguments, and English strings looked up that are not in the catalog.
std::set<std::string> EditorCatalogProblems();
bool EditorCatalogHas(const std::string& english);
std::set<std::string> EditorMissingTranslations();

}  // namespace oe
