#define Uses_TScrollBar
#define Uses_TFrame
#define Uses_TDrawBuffer
#define Uses_TProgram
#define Uses_MsgBox
#define Uses_TKeys
#define __INC_EDITORS_H
#include <tvision/tv.h>

#include <turbo/tpath.h>
#include <turbo/util.h>
#include <turbo/scintilla.h>
#include <turbo/styles.h>
#include <turbo/editstates.h>
#include <turbo/specmodel.h>
#include "editwindow.h"
#include "app.h"
#include "apputils.h"
#include "cmds.h"
#include "search.h"
#include "gotoline.h"
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
using std::ios;

EditorFrame::EditorFrame(const TRect &bounds) noexcept :
    turbo::BasicEditorFrame(bounds)
{
}

void EditorFrame::draw()
{
    turbo::BasicEditorFrame::draw();
    // Draw the "[>]" reveal button left of the zoom icon (which sits at
    // size.x-5..size.x-3). Only meaningful while the frame is active.
    if ((state & sfActive) && size.x > 12)
    {
        TDrawBuffer b;
        TAttrPair cFrame = getColor(0x0503);
        b.moveCStr(0, "[\xE2\x96\xB6]", cFrame); // [►]
        writeLine(size.x - 9, 0, 3, 1, b);
    }
}

void EditorFrame::handleEvent(TEvent &ev)
{
    if (ev.what == evMouseDown && (state & sfActive))
    {
        TPoint m = makeLocal(ev.mouse.where);
        if (m.y == 0 && m.x >= size.x - 9 && m.x <= size.x - 7)
        {
            message(TProgram::application, evCommand, cmRevealInTree, owner);
            clearEvent(ev);
            return;
        }
    }
    turbo::BasicEditorFrame::handleEvent(ev);
}

// ---------------------------------------------------------------------------
// EditorConflictBar -- in-editor git merge-conflict resolution toolbar.

namespace {

// A parsed conflict block, as line indices into the document:
//   <<<<<<<   startLine
//   ours...
//   |||||||   baseLine  (-1 unless diff3 conflict style)
//   base...
//   =======   sepLine
//   theirs...
//   >>>>>>>   endLine
struct ConflictBlock { int startLine, baseLine, sepLine, endLine; };

bool markerAt(const std::string &doc, size_t at, const char *mk) noexcept
{
    return at + 7 <= doc.size() && doc.compare(at, 7, mk) == 0;
}

// Byte offset of the start of each line.
std::vector<size_t> lineStarts(const std::string &doc)
{
    std::vector<size_t> v;
    v.push_back(0);
    for (size_t i = 0; i < doc.size(); ++i)
        if (doc[i] == '\n')
            v.push_back(i + 1);
    return v;
}

std::vector<ConflictBlock> parseConflicts(const std::string &doc,
                                          const std::vector<size_t> &ls)
{
    std::vector<ConflictBlock> out;
    int n = (int) ls.size();
    for (int i = 0; i < n; )
    {
        if (markerAt(doc, ls[i], "<<<<<<<"))
        {
            ConflictBlock b {i, -1, -1, -1};
            for (int j = i + 1; j < n; ++j)
            {
                if (b.sepLine < 0 && b.baseLine < 0 && markerAt(doc, ls[j], "|||||||"))
                    b.baseLine = j;
                else if (b.sepLine < 0 && markerAt(doc, ls[j], "======="))
                    b.sepLine = j;
                else if (markerAt(doc, ls[j], ">>>>>>>")) { b.endLine = j; break; }
            }
            if (b.sepLine >= 0 && b.endLine >= 0)
            {
                out.push_back(b);
                i = b.endLine + 1;
                continue;
            }
        }
        ++i;
    }
    return out;
}

std::string docText(turbo::Editor &ed)
{
    long len = (long) ed.callScintilla(SCI_GETLENGTH, 0U, 0U);
    TStringView sv = turbo::getRangePointer(ed.scintilla, 0, len);
    return std::string(sv.data(), sv.size());
}

long caretLine(turbo::Editor &ed)
{
    long pos = (long) ed.callScintilla(SCI_GETCURRENTPOS, 0U, 0U);
    return (long) ed.callScintilla(SCI_LINEFROMPOSITION, pos, 0U);
}

// The conflict block to act on: the one containing the caret, else the first
// at/after it, else the first.
int targetBlock(const std::vector<ConflictBlock> &blocks, long cl)
{
    for (int i = 0; i < (int) blocks.size(); ++i)
        if (cl >= blocks[i].startLine && cl <= blocks[i].endLine)
            return i;
    for (int i = 0; i < (int) blocks.size(); ++i)
        if (blocks[i].startLine >= cl)
            return i;
    return 0;
}

// Replace the caret's conflict block: kind 0 = ours, 1 = theirs, 2 = both.
void applyChoice(EditorWindow *win, int kind)
{
    auto &ed = win->getEditor();
    std::string doc = docText(ed);
    auto ls = lineStarts(doc);
    auto blocks = parseConflicts(doc, ls);
    if (blocks.empty())
        return;
    const ConflictBlock &bk = blocks[targetBlock(blocks, caretLine(ed))];
    auto byteOf = [&] (int line) -> size_t {
        return line < (int) ls.size() ? ls[line] : doc.size();
    };
    int oursEnd = bk.baseLine >= 0 ? bk.baseLine : bk.sepLine;
    std::string ours   = doc.substr(byteOf(bk.startLine + 1),
                                    byteOf(oursEnd) - byteOf(bk.startLine + 1));
    std::string theirs = doc.substr(byteOf(bk.sepLine + 1),
                                    byteOf(bk.endLine) - byteOf(bk.sepLine + 1));
    std::string repl = kind == 0 ? ours : kind == 1 ? theirs : ours + theirs;
    size_t start = byteOf(bk.startLine), end = byteOf(bk.endLine + 1);
    ed.callScintilla(SCI_BEGINUNDOACTION, 0U, 0U);
    ed.callScintilla(SCI_DELETERANGE, (uptr_t) start, (sptr_t) (end - start));
    ed.callScintilla(SCI_INSERTTEXT, (uptr_t) start, (sptr_t) repl.c_str());
    ed.callScintilla(SCI_ENDUNDOACTION, 0U, 0U);
    ed.callScintilla(SCI_GOTOPOS, (uptr_t) start, 0U);
    ed.callScintilla(SCI_SCROLLCARET, 0U, 0U);
    ed.redraw();
    if (win->conflictBar)
        win->conflictBar->drawView(); // refresh the remaining-count label
}

// Move the caret to the next (dir>0) / previous (dir<0) conflict, wrapping.
void navConflict(EditorWindow *win, int dir)
{
    auto &ed = win->getEditor();
    std::string doc = docText(ed);
    auto ls = lineStarts(doc);
    auto blocks = parseConflicts(doc, ls);
    if (blocks.empty())
        return;
    long cl = caretLine(ed);
    int n = (int) blocks.size(), target = -1;
    if (dir >= 0)
    {
        for (int i = 0; i < n; ++i)
            if (blocks[i].startLine > cl) { target = i; break; }
        if (target < 0) target = 0;       // wrap to first
    }
    else
    {
        for (int i = n - 1; i >= 0; --i)
            if (blocks[i].endLine < cl) { target = i; break; }
        if (target < 0) target = n - 1;   // wrap to last
    }
    ed.callScintilla(SCI_GOTOLINE, (uptr_t) blocks[target].startLine, 0U);
    ed.callScintilla(SCI_SCROLLCARET, 0U, 0U);
    ed.redraw();
}

} // namespace

EditorConflictBar::EditorConflictBar(const TRect &bounds, EditorWindow *aWin) noexcept :
    TView(bounds), win(aWin)
{
    eventMask |= evMouseDown;
}

void EditorConflictBar::draw()
{
    TColorAttr cBar = win ? win->mapColor(turbo::wndFrameActive + 1) : TColorAttr {};
    TColorAttr cBtn = cBar;            // buttons: frame colours reversed, so they
    setFore(cBtn, getBack(cBar));      // read as raised [Label] controls
    setBack(cBtn, getFore(cBar));

    int n = 0;
    if (win)
    {
        std::string doc = docText(win->getEditor());
        n = (int) parseConflicts(doc, lineStarts(doc)).size();
    }

    TDrawBuffer b;
    b.moveChar(0, ' ', cBar, size.x);
    for (int i = 0; i < bCount; ++i) hx0[i] = hx1[i] = -1;

    int x = 1;
    auto button = [&] (Btn id, const char *label) {
        std::string s = "[" + std::string(label) + "]";
        b.moveStr(x, s.c_str(), cBtn);
        hx0[id] = x; hx1[id] = x + (int) s.size();
        x = hx1[id] + 1;
    };
    auto text = [&] (const std::string &s) {
        b.moveStr(x, s.c_str(), cBar);
        x += (int) s.size();
    };

    button(bPrev, "Prev");
    button(bNext, "Next");
    text("  " + std::to_string(n) + (n == 1 ? " conflict   " : " conflicts   "));
    button(bOurs, "Ours");
    button(bTheirs, "Theirs");
    button(bBoth, "Both");
    text("   ");
    button(bResolve, "Mark Resolved");
    text(" ");
    button(bAbort, "Abort Merge");

    writeLine(0, 0, size.x, 1, b);
}

void EditorConflictBar::handleEvent(TEvent &ev)
{
    if (ev.what == evMouseDown && win)
    {
        TPoint m = makeLocal(ev.mouse.where);
        int hit = -1;
        for (int i = 0; i < bCount; ++i)
            if (m.x >= hx0[i] && m.x < hx1[i]) { hit = i; break; }
        switch (hit)
        {
            case bPrev:   navConflict(win, -1); break;
            case bNext:   navConflict(win, +1); break;
            case bOurs:   applyChoice(win, 0); break;
            case bTheirs: applyChoice(win, 1); break;
            case bBoth:   applyChoice(win, 2); break;
            case bResolve:
            {
                std::string doc = docText(win->getEditor());
                if (!parseConflicts(doc, lineStarts(doc)).empty())
                    messageBox("This file still has conflict markers. Resolve "
                               "every conflict, then Mark Resolved.",
                               mfInformation | mfOKButton);
                else
                    message(TProgram::application, evCommand, cmGitResolveFile, win);
                break;
            }
            case bAbort:
                if (messageBox("Abort the merge? This discards the merge and any "
                               "conflict resolution done so far.",
                               mfWarning | mfYesButton | mfNoButton) == cmYes)
                    message(TProgram::application, evCommand, cmGitMergeAbort, nullptr);
                break;
        }
        clearEvent(ev);
        return;
    }
    TView::handleEvent(ev);
}

TFrame *EditorWindow::initFrame(TRect bounds)
{
    return new EditorFrame(bounds);
}

EditorWindow::EditorWindow( const TRect &bounds, TurboEditor &aEditor,
                            active_counter &fileCounter,
                            turbo::SearchSettings &searchSettings,
                            EditorWindowParent &aParent ) noexcept :
    TWindowInit(&EditorWindow::initFrame),
    super(bounds, aEditor),
    listHead(this),
    fileNumber(fileCounter),
    parent(aParent),
    searchState {searchSettings}
{
    // Commands that always get enabled when focusing the editor.
    enabledCmds += cmSave;
    enabledCmds += cmSaveAs;
    enabledCmds += cmToggleWrap;
    enabledCmds += cmToggleLineNums;
    enabledCmds += cmFind;
    enabledCmds += cmReplace;
    enabledCmds += cmGoToLine;
    enabledCmds += cmSearchAgain;
    enabledCmds += cmSearchPrev;
    enabledCmds += cmToggleIndent;
    enabledCmds += cmCloseEditor;
    enabledCmds += cmSelUppercase;
    enabledCmds += cmSelLowercase;
    enabledCmds += cmSelCapitalize;
    enabledCmds += cmToggleComment;
    enabledCmds += cmReplaceOne;
    enabledCmds += cmReplaceAll;
    enabledCmds += cmCompletion;
    enabledCmds += cmSelectNextOccurrence;
    enabledCmds += cmSelectAllOccurrences;
    enabledCmds += cmAddCaretUp;
    enabledCmds += cmAddCaretDown;
    enabledCmds += cmSkipOccurrence;
    enabledCmds += cmUndoSelection;
    enabledCmds += cmSplitSelectionLines;
    enabledCmds += cmCollapseSelection;
    enabledCmds += cmToggleBookmark;
    enabledCmds += cmNextBookmark;
    enabledCmds += cmPrevBookmark;
    enabledCmds += cmToggleBreakpoint;
    enabledCmds += cmToggleFolding;
    enabledCmds += cmFoldAtCursor;
    enabledCmds += cmFoldAll;
    enabledCmds += cmUnfoldAll;
    enabledCmds += cmToggleChangeHistory;
    enabledCmds += cmToggleEdge;

    // Commands that always get disabled when unfocusing the editor.
    disabledCmds += enabledCmds;
    disabledCmds += cmRename;
}

void EditorWindow::shutDown()
{
    parent.removeEditor(*this);
    bottomView = nullptr;
    super::shutDown();
}

void EditorWindow::handleEvent(TEvent &ev)
{
    using namespace turbo;
    auto &editor = getEditor();
    TurboFileDialogs dlgs {parent};
    bool handled = true;
    switch (ev.what)
    {
        case evKeyDown:
            // Ctrl+Space requests completion. We match on the raw key + control
            // modifier rather than a fixed keyCode, since terminals encode it
            // inconsistently (and macOS Option+Space rarely arrives as Alt).
            if ( (ev.keyDown.controlKeyState & kbCtrlShift) &&
                 ev.keyDown.charScan.charCode == ' ' )
            {
                parent.editorRequestCompletion(*this);
                break;
            }
            // Add caret above/below: Ctrl+Alt+Up/Down. Alt dominates the keycode
            // (so it arrives as kbAltUp/kbAltDown) while Ctrl stays in the
            // modifier state, so match on both rather than a fixed keyCode.
            if ( (ev.keyDown.controlKeyState & kbCtrlShift) &&
                 (ev.keyDown.keyCode == kbAltUp || ev.keyDown.keyCode == kbAltDown) )
            {
                if (ev.keyDown.keyCode == kbAltUp) editor.addCaretUp();
                else                               editor.addCaretDown();
                editor.redraw();
                break;
            }
            switch (ev.keyDown.keyCode)
            {
                case kbEsc:
                    if (bottomView)
                        closeBottomView();
                    else if (editor.callScintilla(SCI_GETSELECTIONS, 0U, 0U) > 1)
                    {
                        editor.collapseSelection();
                        editor.redraw();
                    }
                    else
                        handled = false;
                    break;
                default:
                    handled = false;
            }
            break;
        case evCommand:
            switch (ev.message.command)
            {
                case cmSave:
                    parent.editorWillSave(*this);
                    editor.save(dlgs);
                    break;
                case cmSaveAs:
                    editor.saveAs(dlgs);
                    break;
                case cmRename:
                    editor.rename(dlgs);
                    break;
                case cmToggleWrap:
                    editor.wrapping.toggle(editor.scintilla);
                    editor.redraw();
                    break;
                case cmToggleLineNums:
                    editor.lineNumbers.toggle();
                    editor.redraw();
                    break;
                case cmToggleIndent:
                    editor.autoIndent.toggle();
                    break;
                case cmCloseEditor:
                    ev.message.command = cmClose;
                    handled = false;
                    break;
                case cmSelUppercase:
                    editor.uppercase();
                    editor.partialRedraw();
                    break;
                case cmSelLowercase:
                    editor.lowercase();
                    editor.partialRedraw();
                    break;
                case cmSelCapitalize:
                    editor.capitalize();
                    editor.partialRedraw();
                    break;
                case cmToggleComment:
                    editor.toggleComment();
                    editor.partialRedraw();
                    break;
                case cmFind:
                    openBottomView<FindBox>(searchState);
                    break;
                case cmReplace:
                    openBottomView<ReplaceBox>(searchState);
                    break;
                case cmSearchAgain:
                    editor.search(searchState.findText, sdForward, searchState.settingsPreset.get());
                    editor.partialRedraw();
                    break;
                case cmSearchPrev:
                    editor.search(searchState.findText, sdBackwards, searchState.settingsPreset.get());
                    editor.partialRedraw();
                    break;
                case cmSearchIncr:
                    editor.search(searchState.findText, sdForwardIncremental, searchState.settingsPreset.get());
                    editor.partialRedraw();
                    break;
                case cmReplaceOne:
                    editor.replace(searchState.findText, searchState.replaceText, rmReplaceOne, searchState.settingsPreset.get());
                    editor.partialRedraw();
                    break;
                case cmReplaceAll:
                    editor.replace(searchState.findText, searchState.replaceText, rmReplaceAll, searchState.settingsPreset.get());
                    editor.partialRedraw();
                    break;
                case cmClearReplace:
                    editor.clearReplaceIndicator();
                    editor.partialRedraw();
                    break;
                case cmSelectNextOccurrence:
                    editor.selectNextOccurrence();
                    editor.redraw();
                    break;
                case cmSelectAllOccurrences:
                    editor.selectAllOccurrences();
                    editor.redraw();
                    break;
                case cmAddCaretUp:
                    editor.addCaretUp();
                    editor.redraw();
                    break;
                case cmAddCaretDown:
                    editor.addCaretDown();
                    editor.redraw();
                    break;
                case cmSkipOccurrence:
                    editor.skipOccurrence();
                    editor.redraw();
                    break;
                case cmUndoSelection:
                    editor.undoSelection();
                    editor.redraw();
                    break;
                case cmSplitSelectionLines:
                    editor.splitSelectionIntoLines();
                    editor.redraw();
                    break;
                case cmCollapseSelection:
                    editor.collapseSelection();
                    editor.redraw();
                    break;
                case cmToggleBookmark:
                    editor.toggleBookmark();
                    editor.redraw();
                    break;
                case cmToggleBreakpoint:
                    parent.editorToggleBreakpoint(*this, editor.currentLine());
                    break;
                case cmNextBookmark:
                    editor.nextBookmark();
                    editor.redraw();
                    break;
                case cmPrevBookmark:
                    editor.prevBookmark();
                    editor.redraw();
                    break;
                case cmToggleFolding:
                    editor.toggleFolding();
                    editor.redraw();
                    break;
                case cmFoldAtCursor:
                    editor.toggleFoldAtCursor();
                    editor.redraw();
                    break;
                case cmFoldAll:
                    editor.foldAll(true);
                    editor.redraw();
                    break;
                case cmUnfoldAll:
                    editor.foldAll(false);
                    editor.redraw();
                    break;
                case cmToggleChangeHistory:
                    editor.toggleChangeHistory();
                    editor.redraw();
                    break;
                case cmToggleEdge:
                    editor.toggleEdge();
                    editor.redraw();
                    break;
                case cmGoToLine:
                    openBottomView<GoToLineBox>(editor);
                    break;
                case cmCompletion:
                    parent.editorRequestCompletion(*this);
                    break;
                case cmCloseView:
                    if ((handled = bottomView && ev.message.infoPtr == bottomView))
                        closeBottomView();
                    break;
                default:
                    handled = false;
            }
            break;
        default:
            handled = false;
    }
    if (handled)
        clearEvent(ev);
    else
        super::handleEvent(ev);
}

// Warm-brown colours for Lua script windows: two shades (active / passive) shared
// by the frame chrome and the editor's text background, so a script window reads
// as one brown surface that brightens when focused. The active shade equals the
// editor's active text background (mirroring how the blue frame matches the blue
// editor background), and the passive shade is the dimmer inactive tone.
namespace {
constexpr TColorDesired
    cLuaBgActive       = 0x553D1E, // active: editor text + active frame background
    cLuaBgPassive      = 0x3A2A15, // passive: dimmer shade when the window is inactive
    cLuaFrameFgActive  = 0xFFF0D8, // active frame text / box lines (warm white)
    cLuaFrameFgPassive = 0xC9B393, // passive frame text (dim warm)
    cLuaIcon           = 0xE8C07D, // frame icons (gold), on the active brown
    cLuaBarTrough      = 0x2A1E12, // scrollbar trough
    cLuaBarThumb       = 0x6E5230, // scrollbar slider
    cLuaBarArrows      = 0xE8D4B0; // scrollbar arrows

// Spec files (under <project-root>/specs/) are a single deep-violet surface
// -- frame and text, like the Lua windows' brown (D29 revising D12) -- in
// two shades: brighter active, dimmer passive. Icons stay gold, tying the
// purple into the blue/gold palette.
constexpr TColorDesired
    cSpecBgActive       = 0x2A1B4D, // active: deep violet (editor + frame)
    cSpecBgPassive      = 0x1F1838, // passive: darker, desaturated violet
    cSpecFrameFgActive  = 0xE6DFF5, // active frame text / box lines
    cSpecFrameFgPassive = 0xA79BC7, // passive frame text (dim lavender)
    cSpecIcon           = 0xE8C07D, // frame icons (gold), on the active violet
    cSpecBarTrough      = 0x1A1230, // scrollbar trough
    cSpecBarThumb       = 0x5B4691, // scrollbar slider
    cSpecBarArrows      = 0xCBB8F0; // scrollbar arrows
} // namespace

// Window-chrome scheme for Lua script windows: the active chrome with the frame
// and scrollbars recoloured brown. Rebuilt from windowSchemeActive each call so it
// tracks theme edits for the entries it does not override.
static const turbo::WindowColorScheme &luaBrownScheme() noexcept
{
    using namespace turbo;
    static WindowColorScheme brown;
    for (int i = 0; i < WindowPaletteItemCount; ++i)
        brown[i] = windowSchemeActive[i];
    ::setFore(brown[wndFramePassive], cLuaFrameFgPassive);
    ::setBack(brown[wndFramePassive], cLuaBgPassive);
    ::setFore(brown[wndFrameActive], cLuaFrameFgActive);
    ::setBack(brown[wndFrameActive], cLuaBgActive);
    ::setFore(brown[wndFrameIcon], cLuaIcon);
    ::setBack(brown[wndFrameIcon], cLuaBgActive);
    ::setFore(brown[wndScrollBarPageArea], cLuaBarThumb);
    ::setBack(brown[wndScrollBarPageArea], cLuaBarTrough);
    ::setFore(brown[wndScrollBarControls], cLuaBarArrows);
    ::setBack(brown[wndScrollBarControls], cLuaBarTrough);
    return brown;
}

// Window-chrome scheme for spec windows: the active chrome with frame and
// scrollbars recoloured to the spec violet (magenta in classic 16-colour
// mode, which has no purple). Rebuilt from windowSchemeActive each call so
// it tracks theme edits for the entries it does not override.
static const turbo::WindowColorScheme &specPurpleScheme() noexcept
{
    using namespace turbo;
    static WindowColorScheme purple;
    for (int i = 0; i < WindowPaletteItemCount; ++i)
        purple[i] = windowSchemeActive[i];
    if (::getBack(windowSchemeActive[wndFrameActive]).isBIOS())
    {
        ::setFore(purple[wndFramePassive], TColorDesired(uchar(0x7)));
        ::setBack(purple[wndFramePassive], TColorDesired(uchar(0x5)));
        ::setFore(purple[wndFrameActive], TColorDesired(uchar(0xF)));
        ::setBack(purple[wndFrameActive], TColorDesired(uchar(0x5)));
        ::setFore(purple[wndFrameIcon], TColorDesired(uchar(0xE)));
        ::setBack(purple[wndFrameIcon], TColorDesired(uchar(0x5)));
        return purple;
    }
    ::setFore(purple[wndFramePassive], cSpecFrameFgPassive);
    ::setBack(purple[wndFramePassive], cSpecBgPassive);
    ::setFore(purple[wndFrameActive], cSpecFrameFgActive);
    ::setBack(purple[wndFrameActive], cSpecBgActive);
    ::setFore(purple[wndFrameIcon], cSpecIcon);
    ::setBack(purple[wndFrameIcon], cSpecBgActive);
    ::setFore(purple[wndScrollBarPageArea], cSpecBarThumb);
    ::setBack(purple[wndScrollBarPageArea], cSpecBarTrough);
    ::setFore(purple[wndScrollBarControls], cSpecBarArrows);
    ::setBack(purple[wndScrollBarControls], cSpecBarTrough);
    return purple;
}

void EditorWindow::applyActiveStateTheme() noexcept
{
    using namespace turbo;
    // Editor background follows the window's active state, like the tree/output:
    // the unified blue when active, the dimmer passive shade when not. We re-theme
    // from a copy of the active scheme with only the normal-text background swapped
    // (other styles inherit it), so syntax colours are preserved.
    bool active = (state & sfActive) != 0;
    auto &ed = getEditor();
    // Lua script buffers are a single warm-brown surface -- frame and text -- in
    // two shades (brighter active, dimmer passive), so they read as "scripting /
    // configuration" windows. The frame/chrome comes from a brown window scheme;
    // the editor's text background is swapped to the matching shade.
    bool isLuaScript = ed.language == &Language::Lua;
    setScheme(isLuaScript ? &luaBrownScheme()
              : isSpec   ? &specPurpleScheme()
                         : nullptr);
    TColorDesired bg;
    if (isLuaScript)
        bg = active ? cLuaBgActive : cLuaBgPassive;
    else if (isSpec)
    {
        // Specs render on a deep purple surface so a spec is always
        // recognisably a spec; frames keep their normal treatment. Classic
        // 16-colour mode has no purple, so fall back to BIOS magenta there.
        TColorDesired frameBg = ::getBack(windowSchemeActive[wndFrameActive]);
        if (frameBg.isBIOS())
            bg = TColorDesired(uchar(0x5)); // BIOS magenta
        else
            bg = active ? cSpecBgActive : cSpecBgPassive;
    }
    else
        bg = ::getBack(windowSchemeActive[active ? wndFrameActive : wndFramePassive]);
    ColorScheme s;
    for (int i = 0; i < TextStyleCount; ++i)
        s[i] = schemeActive[i];
    ::setBack(s[sNormal], bg);
    applyTheming(ed.lexer, &s, ed.scintilla);
    ed.redraw();
    if (frame)
        frame->drawView(); // repaint the frame with the (possibly brown) scheme
}

void EditorWindow::setState(ushort aState, Boolean enable)
{
    super::setState(aState, enable);
    if (aState == sfActive)
    {
        applyActiveStateTheme(); // dim/undim the editor background
        updateCommands();
        if (enable)
            parent.handleFocus(*this);
        else if ( parent.autoSaveOnFocusLoss() && !filePath().empty() &&
                  !getEditor().inSavePoint() )
        {
            // Auto-save the file when the editor loses focus, if enabled.
            // A non-empty path means save() writes directly without prompting.
            TurboFileDialogs dlgs {parent};
            parent.editorWillSave(*this);
            getEditor().save(dlgs);
        }
    }
}

Boolean EditorWindow::valid(ushort command)
{
    auto &editor = getEditor();
    if (super::valid(command))
    {
        if (command != cmValid)
        {
            TurboFileDialogs dlgs {parent};
            return editor.close(dlgs);
        }
        return true;
    }
    return false;
}

const char* EditorWindow::getTitle(short)
{
    return formatTitle();
}

void EditorWindow::updateCommands() noexcept
{
    if (!filePath().empty())
        enabledCmds += cmRename;
    if (state & sfActive)
        enableCommands(enabledCmds);
    else
        disableCommands(disabledCmds);
}

void EditorWindow::handleNotification(const SCNotification &scn, turbo::Editor &editor)
{
    using namespace turbo;
    super::handleNotification(scn, editor);
    switch (scn.nmhdr.code)
    {
        case SCN_SAVEPOINTREACHED:
            updateCommands();
            parent.handleTitleChange(*this);
            editor.redraw();
            parent.editorSaved(*this);
            break;
        case SCN_MODIFIED:
            if (scn.modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT))
            {
                parent.editorTextChanged(*this);
                if (specBar)
                    specBar->drawView(); // keep the section strip live
            }
            break;
        case SCN_CHARADDED:
            parent.editorCharAdded(*this, scn.ch);
            break;
        case SCN_DWELLSTART:
            parent.editorHoverStart(*this, scn.position);
            break;
        case SCN_DWELLEND:
            parent.editorHoverEnd(*this);
            break;
    }
}

static void growEditor(turbo::Editor &editor, int dy)
{
    turbo::forEachNotNull([&] (TView &v) {
        TRect r = v.getBounds();
        r.b.y += dy;
        v.setBounds(r);
    }, editor.view, editor.leftMargin);
}

void EditorWindow::closeBottomView()
// Pre: 'bottomView' is not null.
{
    int dy = bottomView->size.y + !!(bottomView->options & ofFramed);
    growEditor(editor, dy);
    destroy(bottomView);
    bottomView = nullptr;
    editor.redraw();
}

void EditorWindow::setBottomView(TView *view)
{
    if (bottomView)
        closeBottomView();
    insert(view);
    bottomView = view;
    int dy = -(bottomView->size.y + !!(bottomView->options & ofFramed));
    growEditor(editor, dy);
}

// Move the editor view + line-number margin's TOP edge by 'dy' rows, to free (or
// reclaim) a row at the top of the window for the conflict toolbar.
static void shiftEditorTop(turbo::Editor &editor, int dy)
{
    turbo::forEachNotNull([&] (TView &v) {
        TRect r = v.getBounds();
        r.a.y += dy;
        v.setBounds(r);
    }, editor.view, editor.leftMargin);
}

void EditorWindow::setConflictMode(bool on) noexcept
{
    if (on == (conflictBar != nullptr))
        return; // idempotent: already in the requested state
    if (on)
    {
        TRect r = getExtent();
        r.grow(-1, -1);
        r.b.y = r.a.y + 1;            // single interior row under the title bar
        conflictBar = new EditorConflictBar(r, this);
        conflictBar->growMode = gfGrowHiX; // stretch horizontally, stay at the top
        insert(conflictBar);
        shiftEditorTop(editor, +1);
    }
    else
    {
        shiftEditorTop(editor, -1);
        TObject::destroy(conflictBar);
        conflictBar = nullptr;
    }
    editor.redraw();
}

SpecSectionBar::SpecSectionBar(const TRect &bounds, EditorWindow *aWin) noexcept :
    TView(bounds), win(aWin)
{
}

void SpecSectionBar::draw()
{
    TColorAttr cBar = win ? win->mapColor(turbo::wndFrameActive + 1) : TColorAttr {};
    TDrawBuffer b;
    b.moveChar(0, ' ', cBar, size.x);
    if (win)
    {
        std::string doc = docText(win->getEditor());
        auto sections = turbo::specSections(doc);
        int drafted = 0;
        std::string empties;
        for (auto &s : sections)
        {
            if (!s.empty)
            {
                ++drafted;
                continue;
            }
            if (!empties.empty())
                empties += ", ";
            empties += s.title;
        }
        std::string t = " Sections " + std::to_string(drafted) + "/" +
                        std::to_string(sections.size()) + " drafted";
        if (!empties.empty())
            t += "  |  empty: " + empties;
        if ((int) t.size() > size.x)
        {
            t.resize(size.x);
            // Never cut a UTF-8 sequence in half: stray bytes leak as CP437.
            while (!t.empty() && ((unsigned char) t.back() & 0xC0) == 0x80)
                t.pop_back();
        }
        b.moveStr(0, t.c_str(), cBar);
    }
    writeLine(0, 0, size.x, 1, b);
}

void EditorWindow::setSpecSectionsMode(bool on) noexcept
{
    if (on == (specBar != nullptr))
        return; // idempotent
    if (on)
    {
        TRect r = getExtent();
        r.grow(-1, -1);
        r.a.y += conflictBar ? 1 : 0; // sit under the conflict bar if present
        r.b.y = r.a.y + 1;
        specBar = new SpecSectionBar(r, this);
        specBar->growMode = gfGrowHiX;
        insert(specBar);
        shiftEditorTop(editor, +1);
    }
    else
    {
        shiftEditorTop(editor, -1);
        TObject::destroy(specBar);
        specBar = nullptr;
    }
    editor.redraw();
}

template <class T, class ...Args>
void EditorWindow::openBottomView(Args&& ...args)
{
    auto *view = (T *) message(bottomView, evCommand, T::findCommand, nullptr);
    if (!view)
    {
        TRect r = getExtent().grow(-1, -1);
        r.a.y = r.b.y - T::height;
        view = new T(r, static_cast<Args &&>(args)...);
        view->growMode = gfGrowAll & ~gfGrowLoX;
        view->setState(sfActive, state & sfActive);
        setBottomView(view);
        editor.redraw();
    }
    view->select();
    view->resetCurrent();
}

const char* EditorWindow::formatTitle(ushort flags) noexcept
{
    bool inSavePoint = (flags & tfNoSavePoint) || editor.inSavePoint();
    TitleState titleState {fileNumber.counter, fileNumber.number, inSavePoint, number};
    if (lastTitleState != titleState)
    {
        lastTitleState = titleState;
        TStringView name = filePath().empty() ? "Untitled" : TPath::basename(filePath());
        std::ostringstream os;
        // Lead with the stable 1..9 window number (the Alt-N target) when set,
        // since this Turbo Vision frame style hides the built-in number badge.
        if (number >= 1 && number <= 9)
            os << number << ": ";
        os << name;
        if (fileNumber.number > 1)
            os << " (" << fileNumber.number << ')';
        if (!inSavePoint)
            os << '*';
        title = os.str();
    }
    return title.c_str();
}

void EditorWindow::sizeLimits(TPoint &min, TPoint &max)
{
    super::sizeLimits(min, max);
    if (bottomView)
        min.y = ::max(min.y, bottomView->size.y + !!(bottomView->options & ofFramed) + 3);
}
