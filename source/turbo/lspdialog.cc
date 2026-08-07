#define Uses_TDialog
#define Uses_TInputLine
#define Uses_TLabel
#define Uses_TButton
#define Uses_TCheckBoxes
#define Uses_TSItem
#define Uses_TStaticText
#define Uses_TProgram
#define Uses_TDeskTop
#include <tvision/tv.h>

#include "lspdialog.h"
#include "settings.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

// The languages presented in the dialog, with their built-in default commands
// (shown as a hint when nothing is configured).
struct KnownLang { const char *id; const char *label; const char *def; };

const KnownLang kKnownLangs[] = {
    {"cpp",        "C/C++ (clangd):",        "clangd"},
    {"python",     "Python (pyright):",      "pyright-langserver --stdio"},
    {"rust",       "Rust (rust-analyzer):",  "rust-analyzer"},
    {"go",         "Go (gopls):",            "gopls"},
    {"javascript", "JavaScript (tsserver):", "typescript-language-server --stdio"},
    {"php",        "PHP (intelephense):",    "intelephense --stdio"},
    {"elixir",     "Elixir (expert):",       "expert"},
};

constexpr int kNumLangs = sizeof(kKnownLangs) / sizeof(kKnownLangs[0]);
constexpr int kInputMax = 256;

// Number of editable "extra server" rows in the dialog. Extras beyond this many
// (only settable by hand in ~/.turborc) are preserved untouched on save.
constexpr int kExtraSlots = 4;
constexpr int kNameMax = 64;
constexpr int kLangMax = 96;

// Trim leading/trailing whitespace.
std::string trimmed(const char *s) noexcept
{
    std::string v = s ? s : "";
    size_t a = v.find_first_not_of(" \t");
    size_t b = v.find_last_not_of(" \t");
    return (a == std::string::npos) ? std::string() : v.substr(a, b - a + 1);
}

// Splits a comma/space-separated language list ("php, blade") into ids.
std::vector<std::string> splitLangs(const std::string &s) noexcept
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s)
    {
        if (c == ',' || c == ' ' || c == '\t')
        {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        }
        else
            cur += c;
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

// Copies at most cap-1 chars into a TInputLine's data buffer (size cap),
// terminating within bounds -- writing data[cap] corrupts the heap.
void seedInput(TInputLine *line, const std::string &s, int cap) noexcept
{
    strncpy(line->data, s.c_str(), cap - 1);
    line->data[cap - 1] = '\0';
}

} // namespace

bool executeLspDialog(AppSettings &settings) noexcept
{
    const int W = 80;
    const int langY = 4;                       // first per-language row
    const int extraHdrY = langY + kNumLangs + 1;
    const int extraColY = extraHdrY + 1;
    const int extraY = extraColY + 1;          // first extra-server row
    const int btnY = extraY + kExtraSlots + 1;
    const int height = btnY + 3;               // 2-row buttons + border

    auto *d = new TDialog(TRect(0, 0, W, height), "Language Servers");
    d->options |= ofCentered;

    d->insert(new TStaticText(TRect(2, 2, W - 2, 3),
        "Per-language command; blank = built-in default. Servers must be on PATH."));

    auto *enableBox = new TCheckBoxes(
        TRect(3, 3, 45, 4),
        new TSItem("~E~nable language servers", nullptr));
    d->insert(enableBox);

    // Per-language "primary" servers (override the built-in default).
    std::vector<TInputLine *> lines;
    lines.reserve(kNumLangs);
    for (int i = 0; i < kNumLangs; ++i)
    {
        int y = langY + i;
        d->insert(new TLabel(TRect(2, y, 25, y + 1), kKnownLangs[i].label, nullptr));
        auto *line = new TInputLine(TRect(25, y, W - 2, y + 1), kInputMax);
        d->insert(line);
        lines.push_back(line);
    }

    // Extra servers: run ALONGSIDE the per-language ones, and one server may
    // serve several languages (e.g. a Laravel LSP serving "php blade").
    d->insert(new TStaticText(TRect(2, extraHdrY, W - 2, extraHdrY + 1),
        "Extra servers (run alongside; one server may serve several languages):"));
    d->insert(new TStaticText(TRect(3, extraColY, W - 2, extraColY + 1),
        "name            command                                    languages"));

    std::vector<TInputLine *> nameLines, cmdLines, langLines;
    for (int i = 0; i < kExtraSlots; ++i)
    {
        int y = extraY + i;
        auto *nm = new TInputLine(TRect(3, y, 17, y + 1), kNameMax);
        auto *cm = new TInputLine(TRect(18, y, 62, y + 1), kInputMax);
        auto *lg = new TInputLine(TRect(63, y, W - 2, y + 1), kLangMax);
        d->insert(nm); d->insert(cm); d->insert(lg);
        nameLines.push_back(nm); cmdLines.push_back(cm); langLines.push_back(lg);
    }

    d->insert(new TButton(TRect(W - 24, btnY, W - 14, btnY + 2), "O~K~", cmOK, bfDefault));
    d->insert(new TButton(TRect(W - 13, btnY, W - 3, btnY + 2), "Cancel", cmCancel, bfNormal));

    // Seed the controls from current settings.
    if (settings.lspEnabled)
        enableBox->press(0);
    for (int i = 0; i < kNumLangs; ++i)
        seedInput(lines[i], settings.lspCommandFor(kKnownLangs[i].id), kInputMax);
    for (int i = 0; i < kExtraSlots && i < (int) settings.lspExtraServers.size(); ++i)
    {
        const LspExtraServer &e = settings.lspExtraServers[i];
        std::string langs;
        for (size_t j = 0; j < e.languages.size(); ++j)
            langs += (j ? " " : "") + e.languages[j];
        seedInput(nameLines[i], e.name, kNameMax);
        seedInput(cmdLines[i], e.command, kInputMax);
        seedInput(langLines[i], langs, kLangMax);
    }

    d->selectNext(False);

    bool accepted = (TProgram::deskTop->execView(d) == cmOK);
    if (accepted)
    {
        settings.lspEnabled = enableBox->mark(0);
        for (int i = 0; i < kNumLangs; ++i)
            settings.setLspCommand(kKnownLangs[i].id, trimmed(lines[i]->data));

        // Rebuild the extra servers from the slots, preserving any beyond the
        // editable slots (set by hand in ~/.turborc) so the dialog never drops
        // config it didn't show.
        std::vector<LspExtraServer> extras;
        for (int i = 0; i < kExtraSlots; ++i)
        {
            std::string name = trimmed(nameLines[i]->data);
            std::string cmd = trimmed(cmdLines[i]->data);
            std::string langs = trimmed(langLines[i]->data);
            if (!name.empty() && !cmd.empty())
                extras.push_back({name, cmd, splitLangs(langs)});
        }
        for (size_t i = kExtraSlots; i < settings.lspExtraServers.size(); ++i)
            extras.push_back(settings.lspExtraServers[i]);
        settings.lspExtraServers = std::move(extras);
    }

    TObject::destroy(d);
    return accepted;
}
