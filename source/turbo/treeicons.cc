#include "treeicons.h"

#include <cstdlib>
#include <cstring>

namespace {

// Deliberately few colours: folders gold, skills violet, Lua blue, and "the row's
// own colour" for everything else -- so a tree full of source files stays calm
// rather than turning into a rainbow.
constexpr uint32_t kFolder = 0xE3B341; // gold
constexpr uint32_t kSkill  = 0xC792EA; // violet
constexpr uint32_t kSpecs  = 0x9D7CD8; // deep purple: the spec surface's accent
constexpr uint32_t kLua    = 0x6EA8FE; // blue
constexpr uint32_t kMd     = 0x9CDC8C;
constexpr uint32_t kCpp    = 0x6EC1FF;
constexpr uint32_t kC      = 0x86B6D6;
constexpr uint32_t kPy     = 0xE6C98C;
constexpr uint32_t kJson   = 0xD7BA7D;
constexpr uint32_t kGit    = 0xF05033;

// Nerd Font pictograms, from the private-use area. Written as explicit UTF-8
// bytes so the encoding cannot depend on the compiler's execution charset.
// wcwidth() -- which is what TText::width uses to measure a row -- reports 1 for
// the PUA, so each of these is exactly one column, as the invariant requires.
struct Pictograms {
    const char *folder, *folderOpen, *file, *lua, *skill,
               *md, *cpp, *c, *py, *json, *git;
};

constexpr Pictograms kNerdPics = {
    "\xEF\x81\xBB", // U+F07B  folder
    "\xEF\x81\xBC", // U+F07C  folder-open
    "\xEF\x85\x9B", // U+F15B  file
    "\xEE\x98\xA0", // U+E620  lua
    "\xEF\x80\xAD", // U+F02D  book        (a skill)
    "\xEE\x98\x89", // U+E609  markdown
    "\xEE\x98\x9D", // U+E61D  c++
    "\xEE\x98\xB5", // U+E635  c
    "\xEE\x98\x86", // U+E606  python
    "\xEE\x98\x8B", // U+E60B  json
    "\xEE\x9C\x82", // U+E702  git
};

// The pictogram-free sets draw no icons at all: an absent icon beats a tofu box,
// and iconColumns == 0 collapses the column rather than leaving a blank gutter.
// Folder-vs-file stays unambiguous because the chevron and the name colour carry
// it (see treeNameColor).
constexpr Pictograms kNoPics = { "", "", "", "", "", "", "", "", "", "", "" };

// Rounded box-drawing, matching CpTranslator::BoxDrawing::Rounded (which app.cc
// opts the window frames into), so the tree's connectors match the chrome.
constexpr TreeGlyphs kNerdGlyphs = {
    "\xE2\x94\x82", // U+2502 vertical guide
    "\xE2\x94\x9C", // U+251C tee
    "\xE2\x95\xAD", // U+256D rounded top-left  (the first row of the tree)
    "\xE2\x95\xB0", // U+2570 rounded bottom-left (a last child)
    "\xE2\x94\x80", // U+2500 horizontal arm    (a leaf)
    // The FULL-SIZE triangles, not the SMALL pair (U+25B8/U+25BE): a
    // right-pointing glyph is squeezed by the cell width in a way a
    // down-pointing one is not, so the small right chevron read as a speck next
    // to its own expanded form. U+25B6 (rather than U+25BA, the deliberately
    // smaller "pointer") is drawn full height in ordinary fonts, matching the
    // down triangle. It carries Emoji=Yes but Emoji_Presentation=No, so a
    // conforming terminal defaults it to TEXT presentation at width 1 -- the
    // double-width colour-emoji face only appears with a U+FE0F selector, which
    // we never append -- keeping the one-column-per-glyph invariant below.
    // Both live in Geometric Shapes, which fonts cover near-universally.
    "\xE2\x96\xB6", // U+25B6 chevron, collapsed
    "\xE2\x96\xBC", // U+25BC chevron, expanded
    /*iconColumns*/ 2,
};

// Same connectors (they come from ordinary fonts, not a Nerd Font), no icons.
constexpr TreeGlyphs kUnicodeGlyphs = {
    "\xE2\x94\x82", "\xE2\x94\x9C", "\xE2\x95\xAD", "\xE2\x95\xB0",
    "\xE2\x94\x80", "\xE2\x96\xB6", "\xE2\x96\xBC",
    /*iconColumns*/ 0,
};

// Pure-ASCII connectors, for a terminal that can render nothing else. (The git
// status badges appended to a node's label are a separate matter -- see
// Node::refreshText.)
constexpr TreeGlyphs kAsciiGlyphs = {
    "|", "+", "+", "\\", "-", ">", "v",
    /*iconColumns*/ 0,
};

TreeIconSetId gSet = TreeIconSetId::Unicode; // the safe default until resolved

const Pictograms &pics() noexcept
{
    return gSet == TreeIconSetId::Nerd ? kNerdPics : kNoPics;
}

// Terminals KNOWN to bundle a Nerd Font symbol fallback, so Nerd glyphs are
// guaranteed to resolve with no user configuration. Ghostty ships "Symbols Nerd
// Font" and falls back to it automatically.
//
// Do NOT widen this by scanning for an installed *NerdFont*.ttf: installed is not
// the same as selected-by-the-terminal, and a false positive produces exactly the
// blank boxes this whole scheme exists to avoid. A user on another terminal who
// HAS patched their font just sets tree.icons=nerd.
bool terminalBundlesNerdFont() noexcept
{
    const char *tp = ::getenv("TERM_PROGRAM");
    return tp && strcmp(tp, "ghostty") == 0;
}

} // namespace

void setTreeIconSet(TreeIconSetId id) noexcept
{
    if (id == TreeIconSetId::Auto)
        id = terminalBundlesNerdFont() ? TreeIconSetId::Nerd : TreeIconSetId::Unicode;
    gSet = id;
}

TreeIconSetId treeIconSet() noexcept { return gSet; }

TreeIconSetId parseTreeIconSet(std::string_view n) noexcept
{
    if (n == "nerd")    return TreeIconSetId::Nerd;
    if (n == "unicode") return TreeIconSetId::Unicode;
    if (n == "ascii")   return TreeIconSetId::Ascii;
    return TreeIconSetId::Auto; // including an unrecognised value: never blank
}

const char *treeIconSetName(TreeIconSetId id) noexcept
{
    switch (id)
    {
        case TreeIconSetId::Nerd:    return "nerd";
        case TreeIconSetId::Unicode: return "unicode";
        case TreeIconSetId::Ascii:   return "ascii";
        default:                     return "auto";
    }
}

const TreeGlyphs &treeGlyphs() noexcept
{
    switch (gSet)
    {
        case TreeIconSetId::Nerd:  return kNerdGlyphs;
        case TreeIconSetId::Ascii: return kAsciiGlyphs;
        default:                   return kUnicodeGlyphs;
    }
}

uint32_t treeNameColor(NodeKind kind) noexcept
{
    switch (kind)
    {
        case NodeKind::Project:
        case NodeKind::Dir:        return kFolder;
        case NodeKind::Skill:
        case NodeKind::SkillsHome: return kSkill;
        case NodeKind::Specs:      return kSpecs;
        case NodeKind::LuaHome:    return kLua;
        case NodeKind::File:       break;
    }
    return 0; // inherit the row's text colour
}

TreeIcon treeIconFor(NodeKind kind, std::string_view path, bool expanded) noexcept
{
    const Pictograms &g = pics();
    switch (kind)
    {
        case NodeKind::Project:
        case NodeKind::Dir:
            return { expanded ? g.folderOpen : g.folder, kFolder, false };
        // A home is a container, so it keeps the folder glyph -- but tinted with
        // what it holds, so the two homes are still told apart at a glance.
        case NodeKind::LuaHome:
            return { expanded ? g.folderOpen : g.folder, kLua, false };
        case NodeKind::SkillsHome:
            return { expanded ? g.folderOpen : g.folder, kSkill, false };
        // The spec home is a container too: folder glyph, spec purple.
        case NodeKind::Specs:
            return { expanded ? g.folderOpen : g.folder, kSpecs, false };
        // A skill itself gets the book, open or closed: it is the thing that makes
        // it a skill rather than a plain folder.
        case NodeKind::Skill:
            return { g.skill, kSkill, false };
        case NodeKind::File:
            break;
    }

    size_t slash = path.find_last_of('/');
    std::string_view base = slash == std::string_view::npos
                          ? path : path.substr(slash + 1);

    // Matched on the whole filename: TPath::extname() yields nothing useful for a
    // dotfile, whose leading dot is not an extension separator.
    struct NameIcon { std::string_view name; const char *glyph; uint32_t color; };
    const NameIcon names[] = {
        { ".gitignore",     g.git, kGit },
        { ".gitattributes", g.git, kGit },
        { ".gitmodules",    g.git, kGit },
    };
    for (auto &n : names)
        if (base == n.name)
            return { n.glyph, n.color, false };

    size_t dot = base.find_last_of('.');
    if (dot != std::string_view::npos && dot != 0)
    {
        std::string_view ext = base.substr(dot + 1);
        struct ExtIcon { std::string_view ext; const char *glyph; uint32_t color; };
        const ExtIcon exts[] = {
            { "lua",  g.lua,  kLua  },
            { "md",   g.md,   kMd   }, { "markdown", g.md, kMd },
            { "cc",   g.cpp,  kCpp  }, { "cpp", g.cpp, kCpp },
            { "cxx",  g.cpp,  kCpp  }, { "hpp", g.cpp, kCpp },
            { "h",    g.cpp,  kCpp  }, { "hh",  g.cpp, kCpp },
            { "c",    g.c,    kC    },
            { "py",   g.py,   kPy   },
            { "json", g.json, kJson },
        };
        for (auto &e : exts)
            if (ext == e.ext)
                return { e.glyph, e.color, false };
    }
    return { g.file, 0, /*inheritColor*/ true };
}
