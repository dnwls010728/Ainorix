#pragma once
// The Markdown the Team Chat draws (EditorTeamChat.cpp): inline spans and the block-level checks,
// kept apart from the drawing so tests can read them.
#include <string>
#include <vector>

namespace oe {

enum : unsigned { kMdBold = 1, kMdItalic = 2, kMdCode = 4, kMdStrike = 8, kMdLink = 16 };

struct MarkdownSpan {
    std::string text;
    unsigned style = 0;  // kMd* flags
    std::string target;  // a link's destination
};

// One line of inline Markdown: **bold**, __bold__, *italic*, _italic_, `code`, ~~strike~~ and
// [text](target). A marker without its closing partner stays plain text, and `_` inside a word
// (snake_case) is not emphasis.
std::vector<MarkdownSpan> ParseMarkdownInline(const std::string& line);
bool IsMarkdownRule(const std::string& line);            // ---, *** or ___
bool IsMarkdownTableSeparator(const std::string& line);  // |---|:--:|
std::vector<std::string> MarkdownTableCells(const std::string& line);

}  // namespace oe
