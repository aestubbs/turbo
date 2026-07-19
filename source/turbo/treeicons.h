#ifndef TURBO_TREEICONS_H
#define TURBO_TREEICONS_H

#include <cstdint>
#include <string_view>

// What a file-tree node stands for. Explicit, because the old approach --
// sniffing the kind from a non-empty luaDir/skillDir string -- does not survive
// skills becoming real folders (a skill is a directory *and* opens a file).
enum class NodeKind : unsigned char {
    Project,     // the project wrapper (its path is the project root)
    Dir,         // an ordinary directory
    File,        // an ordinary file
    LuaHome,     // synthetic "Project/Global Lua" group    (its path is a label)
    SkillsHome,  // synthetic "Project/Global Skills" group (its path is a label)
    Skill,       // a skill: a real folder holding a SKILL.md
    Specs,       // the project's specs/ directory: the spec home
};

// Which glyph set the tree draws with.
//
// Glyph coverage is NOT detectable at runtime: the terminal owns the font, there
// is no in-band query for "did that glyph resolve?", and a missing glyph still
// advances the cursor one column -- so not even a cursor-position probe can tell
// a real glyph from a tofu box. Anything that *guesses* a Nerd Font is present
// will render blanks for somebody. Hence:
//
//   Auto     resolve at startup: Nerd only on terminals KNOWN to bundle a Nerd
//            Font symbol fallback, Unicode everywhere else. The default.
//   Nerd     pictograms from the Nerd Font private-use area. Opt-in (or Auto's
//            pick on an allowlisted terminal).
//   Unicode  box-drawing + chevrons, and NO pictograms -- the icon column
//            collapses entirely rather than leaving a blank gutter. Kind is
//            carried by the chevron and the name colour instead.
//   Ascii    as Unicode, but nothing above 0x7F at all.
enum class TreeIconSetId : unsigned char { Auto, Nerd, Unicode, Ascii };

// One row icon. INVARIANT: 'glyph' is UTF-8, NUL-terminated, and exactly ONE
// column wide -- doctree's getGraph() reserves exactly one column for it, and the
// base outline viewer measures that string for the horizontal-scroll limit and
// for the click-to-toggle hit test. An empty glyph means "draw nothing".
struct TreeIcon {
    const char *glyph {""};
    uint32_t    color {0};          // 0xRRGGBB; ignored when inheritColor is set
    bool        inheritColor {true};// draw in the row's own text colour
};

// The connector/disclosure glyphs, and how many columns the icon occupies (2 for
// a set with pictograms -- the glyph plus its trailing gap -- and 0 for a set
// without, so the column collapses instead of sitting blank).
struct TreeGlyphs {
    const char *vert, *tee, *top, *corner, *dash, *chevRight, *chevDown;
    int iconColumns;
};

// Resolves TreeIconSetId::Auto against the terminal; never stores Auto.
void          setTreeIconSet(TreeIconSetId id) noexcept;
TreeIconSetId treeIconSet() noexcept;                          // always resolved
TreeIconSetId parseTreeIconSet(std::string_view name) noexcept;// unknown -> Auto
const char   *treeIconSetName(TreeIconSetId id) noexcept;

const TreeGlyphs &treeGlyphs() noexcept;

// The icon for a node: by kind first, then -- for a plain file -- by extension or
// filename, falling back to a generic file glyph in the row's own colour. Returns
// an empty glyph in the pictogram-free sets.
TreeIcon treeIconFor(NodeKind kind, std::string_view path, bool expanded) noexcept;

// The name colour for a node's kind, or 0 to inherit the row's text colour. This
// applies in EVERY set, and is what keeps the pictogram-free sets readable.
uint32_t treeNameColor(NodeKind kind) noexcept;

#endif // TURBO_TREEICONS_H
