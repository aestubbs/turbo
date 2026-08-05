#define Uses_TProgram
#define Uses_TDeskTop
#define Uses_TObject
#include <tvision/tv.h>

#include "gotoanything.h"
#include "app.h"
#include "editwindow.h"
#include "fuzzypicker.h"
#include "fuzzymatch.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace {

struct Symbol
{
    std::string name;
    std::string kind;
    long line; // 0-based
};

// A selectable destination. Either jump within an already-open editor (editor
// set, e.g. ':line' / '@symbol' in the active file) or open a path on disk.
struct GotoTarget
{
    std::string path;       // absolute; also used for the preview
    long line;              // 0-based; -1 = top of file
    EditorWindow *editor;   // non-null = jump in this editor instead of opening
};

// Strip a 'rootPath/' prefix so files display relative to the workspace root.
std::string relativeTo(const std::string &root, const std::string &abs) noexcept
{
    if (!root.empty() && abs.size() > root.size() + 1 &&
        abs.compare(0, root.size(), root) == 0 &&
        (abs[root.size()] == '/' || abs[root.size()] == '\\'))
        return abs.substr(root.size() + 1);
    return abs;
}

std::string basenameOf(const std::string &s) noexcept
{
    size_t slash = s.find_last_of("/\\");
    return slash == std::string::npos ? s : s.substr(slash + 1);
}

std::string dirOf(const std::string &s) noexcept
{
    size_t slash = s.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : s.substr(0, slash);
}

// Lightweight, language-agnostic symbol scan: catches keyword-introduced
// definitions (class/struct/def/fn/func/...) and brace-opening C-style function
// definitions. Heuristic, not a parser -- enough to jump around a file fast.
void scanSymbols(const std::string &text, std::vector<Symbol> &out) noexcept
{
    static const std::regex reKw(
        R"((class|struct|enum|union|interface|trait|namespace|module|impl|object|protocol)\s+([A-Za-z_][A-Za-z0-9_]*))");
    static const std::regex reFn(
        R"((def|fn|func|function|sub|proc|fun|method)\s+([A-Za-z_][A-Za-z0-9_]*))");
    // Function definition: an identifier followed by a parameter list, with the
    // opening brace either on this line (K&R) or the next (Allman). A trailing
    // ';' (declaration/call) is excluded because then nothing but ';' follows
    // the ')', which can't match the optional-brace-then-end below.
    static const std::regex reCfn(
        R"(^\s*[A-Za-z_~][A-Za-z0-9_\s\*&:<>,\[\]]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?(?:final\s*)?\{?\s*$)");
    static const std::set<std::string> control = {
        "if", "for", "while", "switch", "catch", "return", "else", "do",
        "sizeof", "case", "throw", "new", "delete", "and", "or" };

    long line = 0;
    size_t start = 0;
    const size_t maxLines = 20000;
    while (start <= text.size() && line < (long) maxLines)
    {
        size_t nl = text.find('\n', start);
        std::string s = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        std::smatch m;
        if (std::regex_search(s, m, reKw))
            out.push_back({ m[2].str(), m[1].str(), line });
        else if (std::regex_search(s, m, reFn))
            out.push_back({ m[2].str(), m[1].str(), line });
        else if (std::regex_search(s, m, reCfn))
        {
            std::string name = m[1].str();
            if (!control.count(name))
                out.push_back({ name, "fn", line });
        }
        if (nl == std::string::npos)
            break;
        start = nl + 1;
        ++line;
    }
}

// Read the active editor's current buffer (includes unsaved edits).
std::string activeBufferText(EditorWindow *w) noexcept
{
    if (!w)
        return {};
    auto &ed = w->getEditor();
    long len = (long) ed.callScintilla(SCI_GETLENGTH, 0U, 0U);
    if (len <= 0)
        return {};
    const char *p = (const char *) ed.callScintilla(SCI_GETCHARACTERPOINTER, 0U, 0U);
    return p ? std::string(p, (size_t) len) : std::string();
}

} // namespace

void runGotoAnything(TurboApp &app) noexcept
{
    if (!app.docTree)
        return;

    // File source: every file in the (already-scanned) project tree.
    std::vector<std::string> files;
    app.docTree->tree->collectFilePaths(files);
    const std::string &root = app.docTree->tree->rootPath;

    // Precompute display strings once.
    std::vector<std::string> rels, names, dirs;
    rels.reserve(files.size());
    names.reserve(files.size());
    dirs.reserve(files.size());
    for (auto &f : files)
    {
        std::string rel = relativeTo(root, f);
        rels.push_back(rel);
        names.push_back(basenameOf(rel));
        dirs.push_back(dirOf(rel));
    }

    EditorWindow *active = app.MRUlist.empty() ? nullptr : app.MRUlist.next->self;
    std::string activePath = active ? active->filePath() : std::string();
    std::string activeRel = activePath.empty() ? std::string("Untitled")
                                               : relativeTo(root, activePath);

    std::vector<Symbol> symbols;
    if (active)
        scanSymbols(activeBufferText(active), symbols);

    int64_t now = (int64_t) ::time(nullptr);

    std::vector<GotoTarget> targets;

    auto provider = [&] (const std::string &raw) -> std::vector<FuzzyPicker::Row>
    {
        targets.clear();
        std::vector<FuzzyPicker::Row> out;

        // Mode by leading sigil.
        if (!raw.empty() && raw[0] == ':')
        {
            long ln = std::atol(raw.c_str() + 1);
            if (active && ln >= 1)
            {
                targets.push_back({ activePath, ln - 1, active });
                out.push_back({ "Go to line " + std::to_string(ln), activeRel, 0, false });
            }
            return out;
        }
        if (!raw.empty() && raw[0] == '@')
        {
            std::string needle = raw.substr(1);
            std::vector<std::pair<int, int>> scored; // (score, symbolIndex)
            for (int i = 0; i < (int) symbols.size(); ++i)
            {
                int s = 0;
                if (fuzzy::fuzzyScore(needle, symbols[i].name, &s))
                    scored.push_back({ s, i });
            }
            std::sort(scored.begin(), scored.end(),
                      [] (auto &a, auto &b) { return a.first > b.first; });
            for (auto &sc : scored)
            {
                const Symbol &sym = symbols[sc.second];
                int payload = (int) targets.size();
                targets.push_back({ activePath, sym.line, active });
                out.push_back({ sym.name,
                                sym.kind + "  :" + std::to_string(sym.line + 1),
                                payload, false });
            }
            return out;
        }

        // File mode, with an optional trailing ':N'.
        std::string needle = raw;
        long lineNo = -1;
        size_t colon = raw.rfind(':');
        if (colon != std::string::npos && colon > 0 && colon + 1 < raw.size())
        {
            bool digits = true;
            for (size_t i = colon + 1; i < raw.size(); ++i)
                if (!std::isdigit((unsigned char) raw[i])) { digits = false; break; }
            if (digits)
            {
                lineNo = std::atol(raw.c_str() + colon + 1) - 1;
                needle = raw.substr(0, colon);
            }
        }

        std::vector<std::pair<int, int>> scored; // (score, fileIndex)
        scored.reserve(files.size());
        for (int i = 0; i < (int) files.size(); ++i)
        {
            int s = 0;
            if (!fuzzy::fuzzyScore(needle, rels[i], &s))
                continue;
            s += app.frecency.score(files[i], now);
            scored.push_back({ s, i });
        }
        std::sort(scored.begin(), scored.end(), [&] (auto &a, auto &b) {
            if (a.first != b.first)
                return a.first > b.first;
            return rels[a.second].size() < rels[b.second].size();
        });
        const int kMaxRows = 400;
        int shown = std::min((int) scored.size(), kMaxRows);
        for (int k = 0; k < shown; ++k)
        {
            int i = scored[k].second;
            int payload = (int) targets.size();
            targets.push_back({ files[i], lineNo, nullptr });
            out.push_back({ names[i], dirs[i], payload, false });
        }
        return out;
    };

    TRect ext = TProgram::deskTop->getExtent();
    int w = std::min(100, (int) ext.b.x - 4);
    int h = std::min(26, (int) ext.b.y - 4);
    TRect r(0, 0, w, h);
    r.move((ext.b.x - w) / 2, (ext.b.y - h) / 4);

    auto *picker = new FuzzyPicker(r, "Goto Anything", provider, /*withPreview=*/true);
    picker->onHighlight = [&] (int payload) {
        if (payload >= 0 && payload < (int) targets.size())
            picker->setPreview(targets[payload].path, targets[payload].line);
    };

    int payload = picker->run();
    if (payload >= 0 && payload < (int) targets.size())
    {
        GotoTarget t = targets[payload];
        if (t.editor)
        {
            t.editor->focus();
            auto &ed = t.editor->getEditor();
            if (t.line >= 0)
            {
                ed.callScintilla(SCI_GOTOLINE, t.line, 0U);
                ed.callScintilla(SCI_SCROLLCARET, 0U, 0U);
                ed.redraw();
            }
        }
        else
            app.openOrFocus(t.path, t.line, /*userInitiated=*/true);
    }
    TObject::destroy(picker);
}
