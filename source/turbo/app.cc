#define Uses_TApplication
#define Uses_MsgBox
#define Uses_TBackground
#define Uses_TDeskTop
#define Uses_TKeys
#define Uses_TMenuBar
#define Uses_TMenuItem
#define Uses_TStatusDef
#define Uses_TStatusItem
#define Uses_TStatusLine
#define Uses_TSubMenu
#define Uses_TWindow
#define Uses_TFrame
#define Uses_TFileDialog
#define Uses_TChDirDialog
#define Uses_TIndicator
#define Uses_TStaticText
#define Uses_TParamText
#define Uses_TScreen
#define Uses_TButton
#define Uses_TDrawBuffer
#define Uses_TMenu
#define Uses_TEventQueue
#include <tvision/tv.h>

#include "app.h"
#include "help.h"
#include "apputils.h"
#include "editwindow.h"
#include "widgets.h"
#include "listviews.h"
#include "doctree.h"
#include "lspmanager.h"
#include "lspdialog.h"
#include "dapmanager.h"
#include "debugdialog.h"
#include "gitmanager.h"
#include "gitdialog.h"
#include "luamanager.h"
#include "menucheck.h"
#include "terminal.h"
#include "agentconfig.h"
#include "mcpserver.h"
#include "themedialog.h"
#include "theme.h"
#include "commandpalette.h"
#include "gotoanything.h"
#include "builddialog.h"
#include "specdialog.h"
#include <turbo/specmodel.h>
#include <turbo/fileeditor.h>
#include <turbo/tpath.h>
#include <tvision/internal/codepage.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>

using namespace Scintilla;

// Defined further down; forward-declared so the Lua helpers above it can trim
// user-entered names too.
static std::string trimmed(const char *s);

// Branch indicator that lives at the right of the menu bar. It draws the current
// branch (icon + name) like the clock used to, and a click opens a popup listing
// the other branches (built fresh each time, so there is no persistent item list
// to keep in sync). The list and the switch logic live in TurboApp.
struct BranchView : public TView
{
    std::string text {"\xE2\x8E\x87 \xE2\x80\xA6"}; // "⎇ …" until git status loads

    BranchView(const TRect &r) noexcept : TView(r)
    {
        eventMask |= evMouseDown;
        growMode = gfGrowLoX | gfGrowHiX; // hug the right edge on resize
    }

    void draw() override
    {
        TDrawBuffer buf;
        TColorAttr c = getColor(2); // same menu-row colour the clock used
        buf.moveChar(0, ' ', c, size.x);
        buf.moveStr(0, text.c_str(), c);
        writeLine(0, 0, size.x, 1, buf);
    }

    void setText(const char *t) noexcept
    {
        if (text != t)
        {
            text = t;
            drawView();
        }
    }

    void handleEvent(TEvent &ev) override
    {
        TView::handleEvent(ev);
        if (ev.what == evMouseDown)
        {
            // Anchor the popup at this view's top-left; popupMenu drops it one row
            // down and nudges it left to stay on screen.
            TPoint p = makeGlobal(TPoint {0, 0});
            if (auto *app = (TurboApp *) TProgram::application)
                app->showBranchMenu(p);
            clearEvent(ev);
        }
    }
};

// Desktop background colour shown behind all windows (a solid fill, replacing
// Turbo Vision's \xB0 shaded pattern).
static constexpr TColorDesired deskBackground = 0x0A0F1E;

// A plain solid-colour desktop background.
struct TurboBackground : public TBackground
{
    TurboBackground(const TRect &bounds) noexcept : TBackground(bounds, ' ') {}

    void draw() override
    {
        TDrawBuffer b;
        TColorAttr c {deskBackground, deskBackground};
        b.moveChar(0, ' ', c, size.x);
        writeLine(0, 0, size.x, size.y, b);
    }
};

// A desktop whose background is our solid-colour view.
struct TurboDeskTop : public TDeskTop
{
    TurboDeskTop(const TRect &bounds) noexcept :
        TDeskInit(&TurboDeskTop::initBackground),
        TDeskTop(bounds)
    {
    }

    static TBackground *initBackground(TRect bounds)
    {
        return new TurboBackground(bounds);
    }
};

TDeskTop *TurboApp::initDeskTop(TRect r)
{
    // Leave room for the menu bar (top row) and status line (bottom row), just
    // as TProgram::initDeskTop does -- otherwise the desktop covers the full
    // screen and hides them (and the windows' title/bottom bars).
    r.a.y++;
    r.b.y--;
    return new TurboDeskTop(r);
}

// Dark, high-contrast palette for the menu bar and the status line. Both resolve
// their colours through application-palette indices 2..7 (TMenuView's cpMenuView
// and TStatusLine's cpStatusLine), so we copy the default app palette and recolor
// just those six entries. The default grey-on-grey made disabled items (greyed
// menu commands, inactive status hints) almost invisible; this gives them a
// clearly readable mid-tone on a dark slate, matching the editor chrome.
TPalette &TurboApp::getPalette() const
{
    if (useClassicColors(settings))
        // Classic 16-colour mode: the stock Turbo Vision app palette already is
        // the authentic BIOS look (blue app, cyan menu highlights, grey
        // dialogs). Its BIOS attributes render through the terminal's own
        // 16-colour palette, so colours stay distinct on weak terminals. Called
        // each redraw, so toggling colour mode swaps palettes live.
        return TProgram::getPalette();

    static TPalette pal = [this] {
        TPalette p = TProgram::getPalette(); // copy of the default app palette
        p[2] = TColorAttr(TColorRGB(0xD7DCE8), TColorRGB(0x222A3D)); // normal item
        p[3] = TColorAttr(TColorRGB(0x8A93A8), TColorRGB(0x222A3D)); // disabled item
        p[4] = TColorAttr(TColorRGB(0xF2A65A), TColorRGB(0x222A3D)); // hotkey letter
        p[5] = TColorAttr(TColorRGB(0xFFFFFF), TColorRGB(0x2E6FD6)); // selected item
        p[6] = TColorAttr(TColorRGB(0xC2C9DC), TColorRGB(0x2E6FD6)); // selected disabled
        p[7] = TColorAttr(TColorRGB(0xFFD27A), TColorRGB(0x2E6FD6)); // selected hotkey

        // Dialog boxes (the gray-dialog block: app palette 32..63, in cpDialog
        // entry order). The stock entries are BIOS colours -- light-gray body,
        // green buttons -- resolved through the terminal's 16-colour table, so
        // dialogs clashed with the fixed-RGB chrome and the default button
        // could be unreadable. Recolour the block to the app's dark scheme:
        // a slate body like the menu bar, controls at rest on the passive
        // window-frame blue, the default button on the active window-frame
        // blue, and the focused button on the menus' selection blue. The three
        // button blues are kept in one family because the hotkey-letter entry
        // (45) is shared across button states, so its cell shows through on
        // the default/focused button. TFrame fills the dialog interior with
        // the frame attribute, so every body-level background must equal the
        // active frame's.
        const TColorRGB
            dlgBody {0x303648}, // interior + frame (lighter than the menu bar)
            dlgLine {0xD7DCE8}, // frame lines, title, prominent text
            dlgText {0xE0E6F8}, // static text
            dlgSoft {0xB8C2E0}, // label text
            dlgDim  {0x8A93A8}, // passive frame / disabled foregrounds
            dlgGold {0xE8C07D}, // hotkey letters
            ctlBg   {0x16335E}, // controls at rest (passive frame blue)
            ctlHot  {0x1E4D8C}, // engaged control (active frame blue)
            ctlSel  {0x2E6FD6}, // focused highlight (menu selection blue)
            ctlDis  {0x1A2540}, // disabled control background
            trough  {0x1A2E52}, // scrollbar trough
            thumb   {0x3A5C92}, // scrollbar slider / dividers
            arrows  {0xD0DEFF}, // scrollbar + input-line overflow arrows
            teal    {0x7FE0B0}, // frame icons / history sides
            white   {0xFFFFFF},
            shadow  {0x0A1020}; // button shadow (drawn as fg half-block glyphs)
        p[32] = TColorAttr(dlgDim,  dlgBody); // frame passive
        p[33] = TColorAttr(dlgLine, dlgBody); // frame active (+ interior fill)
        p[34] = TColorAttr(teal,    dlgBody); // frame icon
        p[35] = TColorAttr(thumb,   trough);  // scrollbar page area
        p[36] = TColorAttr(arrows,  trough);  // scrollbar controls
        p[37] = TColorAttr(dlgText, dlgBody); // static text
        p[38] = TColorAttr(dlgSoft, dlgBody); // label normal
        p[39] = TColorAttr(white,   dlgBody); // label selected
        p[40] = TColorAttr(dlgGold, dlgBody); // label shortcut
        p[41] = TColorAttr(dlgLine, ctlBg);   // button normal
        p[42] = TColorAttr(white,   ctlHot);  // button default
        p[43] = TColorAttr(white,   ctlSel);  // button selected
        p[44] = TColorAttr(dlgDim,  ctlDis);  // button disabled
        p[45] = TColorAttr(dlgGold, ctlBg);   // button shortcut
        // The shadow attr's background must be the body: TButton paints its
        // left column and the start of its bottom row with this attr to erase
        // the face colour there; the visible offset shadow is the half-block
        // glyphs drawn with the foreground on the bottom and right.
        p[46] = TColorAttr(shadow,  dlgBody); // button shadow
        p[47] = TColorAttr(dlgLine, dlgBody); // cluster normal (check/radio)
        p[48] = TColorAttr(white,   ctlHot);  // cluster selected
        p[49] = TColorAttr(dlgGold, dlgBody); // cluster shortcut
        p[50] = TColorAttr(dlgText, ctlBg);   // input line normal
        p[51] = TColorAttr(white,   ctlHot);  // input line focused
        p[52] = TColorAttr(arrows,  ctlBg);   // input line arrows
        p[53] = TColorAttr(dlgText, ctlBg);   // history arrow
        p[54] = TColorAttr(teal,    dlgBody); // history sides
        p[55] = TColorAttr(thumb,   trough);  // history scrollbar page area
        p[56] = TColorAttr(arrows,  trough);  // history scrollbar controls
        p[57] = TColorAttr(dlgText, ctlBg);   // list viewer normal
        p[58] = TColorAttr(white,   ctlSel);  // list viewer focused
        p[59] = TColorAttr(white,   ctlHot);  // list viewer selected
        p[60] = TColorAttr(thumb,   dlgBody); // list viewer divider
        p[61] = TColorAttr(dlgText, dlgBody); // info pane
        p[62] = TColorAttr(dlgDim,  dlgBody); // cluster disabled
        return p;
    }();
    return pal;
}

TurboApp::TurboApp(int argc, const char *argv[]) noexcept :
    TProgInit( &TurboApp::initStatusLine,
               &TurboApp::initMenuBar,
               &TurboApp::initDeskTop
             ),
    argc(argc),
    argv(argv)
{
    // Opt into the fork's modern rounded/single box-drawing for all chrome
    // (frames, menus, file-tree connectors). The library default is faithful CP437.
    tvision::CpTranslator::setBoxDrawing(tvision::CpTranslator::BoxDrawing::Rounded);
    loadSettings(settings);
    // Resolve the file tree's glyph set before the tree is built. "auto" (the
    // default) only picks the Nerd Font pictograms on a terminal known to bundle a
    // Nerd Font fallback: glyph coverage cannot be probed at runtime, and guessing
    // wrong renders blank boxes rather than icons.
    setTreeIconSet(parseTreeIconSet(settings.treeIcons));
    frecency.load();
    // Fold any saved colour overrides onto the built-in 24-bit defaults before
    // the first editor is created, so new editors theme from the right scheme.
    applyThemeFromSettings(settings);
    lsp = std::make_unique<LspManager>();
    configureLsp();
    initDap();
    git = std::make_unique<GitManager>();
    watcher = std::make_unique<turbo::FileWatcher>();
    // Watch the global skills home too: it sits outside the project, so the
    // project watcher never sees a skill added or removed there.
    if (const char *h = ::getenv("HOME"))
        globalSkillsDir = std::string(h) + "/.claude/skills";
    skillsWatcher = std::make_unique<turbo::FileWatcher>();
    startGlobalSkillsWatch();

    TCommandSet ts;
    ts += cmSave;
    ts += cmSaveAs;
    ts += cmRename;
    ts += cmToggleWrap;
    ts += cmToggleLineNums;
    ts += cmFind;
    ts += cmReplace;
    ts += cmGoToLine;
    ts += cmSearchAgain;
    ts += cmSearchPrev;
    ts += cmToggleIndent;
    ts += cmCloseEditor;
    ts += cmSelUppercase;
    ts += cmSelLowercase;
    ts += cmSelCapitalize;
    ts += cmToggleComment;
    ts += cmCompletion;
    ts += cmSelectNextOccurrence;
    ts += cmSelectAllOccurrences;
    ts += cmAddCaretUp;
    ts += cmAddCaretDown;
    ts += cmSkipOccurrence;
    ts += cmUndoSelection;
    ts += cmSplitSelectionLines;
    ts += cmCollapseSelection;
    ts += cmToggleBookmark;
    ts += cmNextBookmark;
    ts += cmPrevBookmark;
    ts += cmToggleFolding;
    ts += cmFoldAtCursor;
    ts += cmFoldAll;
    ts += cmUnfoldAll;
    ts += cmToggleChangeHistory;
    ts += cmToggleEdge;
    ts += cmUndo;
    ts += cmRedo;
    ts += cmCut;
    ts += cmCopy;
    ts += cmPaste;
    disableCommands(ts);

    // Actions that only make sense when there is at least one editor.
    editorCmds += cmEditorNext;
    editorCmds += cmEditorPrev;
    editorCmds += cmTreeNext;
    editorCmds += cmTreePrev;
    editorCmds += cmCloseAll;
    editorCmds += cmCloseEditor;
    disableCommands(editorCmds);

    // Clock: bottom-right of the status line (out of the way; the menu-bar right
    // is now used by the branch indicator).
    TRect ext = getExtent();
    {
        TRect r {ext.b.x - 9, ext.b.y - 1, ext.b.x, ext.b.y};
        clock = new TClockView(r);
        clock->growMode = gfGrowLoX | gfGrowHiX | gfGrowLoY | gfGrowHiY;
        insert(clock);
    }
    // Branch indicator: top-right of the menu bar. refreshBranchView() sizes it
    // to the current branch name; this is just an initial placeholder slot.
    {
        TRect r {ext.b.x - 12, ext.a.y, ext.b.x, ext.a.y + 1};
        branchView = new BranchView(r);
        insert(branchView);
    }

    // Create the document tree view
    {
        TRect r = deskTop->getExtent();
        // Try to make it between 22 and 30 columns wide, and try to leave
        // at least 82 empty columns on screen (so that an editor view is
        // at least ~80 columns by default).
        if (r.b.x > 22)
            r.a.x = r.b.x - min(max(r.b.x - 82, 22), 30);
        docTree = new DocumentTreeWindow(r, &docTree);
        // Docked side pane: no move/grow/zoom (so no bottom-right resize handle);
        // its left border is the resize handle and the close box toggles it off.
        docTree->flags = wfClose;
        // The grow mode assumes it's placed on the right side of the screen.
        // Greater flexibility would require some trick or a dedicated class
        // for side views.
        docTree->growMode = gfGrowLoX | gfGrowHiX | gfGrowHiY;
        docTree->setState(sfShadow, False);
        // Drag the left border to set the width; the editors beside it re-layout.
        docTree->onResizeTo = [this] (int borderScreenX) {
            setTreeWidth(deskTop->getExtent().b.x - borderScreenX);
        };
        deskTop->insert(docTree);
        // Show by default only on large terminals.
        if (deskTop->size.x - docTree->size.x < 82)
            docTree->hide();
    }

    // Output pane (Build/Run): a bordered window docked across the bottom of the
    // editor area, created hidden and shown on demand. Docked the same way the
    // file tree is, just on the Y axis.
    {
        outputWin = new OutputWindow(outputBounds(), &outputWin);
        outputWin->growMode = gfGrowLoY | gfGrowHiY | gfGrowHiX;
        outputWin->setState(sfShadow, False);
        // Clicking / Entering an error line jumps to the source.
        outputWin->view->onActivate = [this] (const std::string &file, long line) {
            openOrFocus(file, line);
        };
        // Activating a Call Stack frame jumps to it and moves the current-line
        // marker there (re-scoping the Variables panel follows in M3b).
        if (outputWin->stackView)
            outputWin->stackView->onSelect = [this] (int idx) {
                if (!outputWin || !outputWin->stackView)
                    return;
                auto &frames = outputWin->stackView->frames;
                if (idx < 0 || idx >= (int) frames.size())
                    return;
                const StackFrameItem &fr = frames[idx];
                if (!fr.file.empty() && fr.line >= 0)
                {
                    clearDebugCurrentLine();
                    openOrFocus(fr.file, fr.line);
                    if (auto *w = focusedEditor())
                        w->getEditor().setCurrentLine(fr.line);
                }
                if (dap)
                    dap->fetchScopes(fr.frameId); // re-scope Variables to this frame
            };
        // Expanding a Variables node lazily fetches its children.
        if (outputWin->varsView)
            outputWin->varsView->onExpand = [this] (int variablesReference, int token) {
                if (!dap)
                    return;
                dap->fetchVariables(variablesReference,
                    [this, token] (std::vector<VariableInfo> vars) {
                        if (!outputWin || !outputWin->varsView)
                            return;
                        std::vector<VarItem> items;
                        items.reserve(vars.size());
                        for (const VariableInfo &v : vars)
                        {
                            std::string text = v.name;
                            if (!v.value.empty())
                                text += " = " + v.value;
                            items.push_back(VarItem{text, v.variablesReference});
                        }
                        outputWin->varsView->fulfill(token, items);
                    });
            };
        // Dragging the pane's top border resizes it. The bottom is anchored to
        // the desktop bottom, so the height is (desktop bottom - dragged row).
        outputWin->onResizeTo = [this] (int borderScreenY) {
            setOutputPaneHeight(deskTop->getBounds().b.y - borderScreenY);
        };
        deskTop->insert(outputWin);
        outputWin->hide();
    }

    // Pipe git's (otherwise swallowed) command output into the output pane,
    // revealing it on demand. Runs on the main thread from git->pump().
    git->setOutputSink([this] (const std::string &label, int code,
                               const std::string &output) {
        reportGitOutput(label, code, output);
    });
}

// Builds 'count' placeholder items for the recent-windows section of the Windows
// menu. They start disabled with a blank label; refreshWindowList() fills in the
// names of the most-recently-used editor windows at runtime. 'count' tracks the
// number of open editors, so there are never trailing empty rows.
static TMenuItem &recentWindowsItems(int count)
{
    TMenuItem *head = nullptr;
    TMenuItem *tail = nullptr;
    for (int i = 0; i < count; ++i)
    {
        auto *item = new TMenuItem(" ", cmWindowBase + i, kbNoKey, hcNoContext);
        item->disabled = True;
        if (!head)
            head = tail = item;
        else { tail->append(item); tail = item; }
    }
    return *head;
}

// Builds 'count' placeholder items for the tool-toggle section of the Run menu.
// Each starts with a blank label; fillToolMenuLabels() writes the configured
// tool names and refreshMenuChecks() ticks the ones whose process is running.
static TMenuItem &toolMenuItems(int count)
{
    TMenuItem *head = nullptr;
    TMenuItem *tail = nullptr;
    for (int i = 0; i < count; ++i)
    {
        auto *item = new TMenuItem("  ", cmToolBase + i, kbNoKey, hcNoContext);
        if (!head)
            head = tail = item;
        else { tail->append(item); tail = item; }
    }
    return *head;
}

TMenuBar *TurboApp::initMenuBar(TRect r)
{
    // No editors open and no project (so no tools) yet: zero dynamic slots.
    return makeMenuBar(r, 0, 0);
}

TMenuBar *TurboApp::makeMenuBar(TRect r, int recentCount, int toolCount)
{
    r.b.y = r.a.y+1;

    // Build the Windows submenu separately so the recent-windows list can be
    // sized to the number of open editors. The file-tree toggle lives here,
    // beside the in-tree navigation.
    TSubMenu &windows = *new TSubMenu( "~W~indows", kbAltW ) +
        *new TMenuItem( "~Z~oom", cmZoom, kbF5, hcNoContext, "F5" ) +
        *new TMenuItem( "~R~esize/move",cmResize, kbCtrlF5, hcNoContext, "Ctrl-F5" ) +
        *new TMenuItem( "~N~ext", cmEditorNext, kbF6, hcNoContext, "F6" ) +
        *new TMenuItem( "~P~revious", cmEditorPrev, kbShiftF6, hcNoContext, "Shift-F6" ) +
        *new TMenuItem( "~C~lose", cmClose, kbAltF3, hcNoContext, "Alt-F3" ) +
        newLine() +
        *new TMenuItem( "File ~T~ree View", cmToggleTree, kbNoKey, hcNoContext ) +
        *new TMenuItem( "Previous (in tree)", cmTreePrev, kbAltUp, hcNoContext, "Alt-Up" ) +
        *new TMenuItem( "Next (in tree)", cmTreeNext, kbAltDown, hcNoContext, "Alt-Down" );
    if (recentCount > 0)
        windows + newLine() + recentWindowsItems(recentCount);

    // Build the Run submenu separately so the per-tool toggles (one checkable
    // item per configured tool process) can be spliced in only when tools exist,
    // bracketed by separators, just before the Show/Hide Output item.
    TSubMenu &run = *new TSubMenu( "~R~un", kbAltR ) +
        *new TMenuItem( "~B~uild", cmBuild, kbF7, hcNoContext, "F7" ) +
        *new TMenuItem( "~R~un", cmRun, kbNoKey, hcNoContext ) +
        *new TMenuItem( "~T~est", cmTest, kbNoKey, hcNoContext ) +
        *new TMenuItem( "~S~top", cmStop, kbNoKey, hcNoContext ) +
        newLine() +
        *new TMenuItem( "~C~onfigure...", cmBuildConfig, kbNoKey, hcNoContext ) +
        *new TMenuItem( "Too~l~s...", cmToolsConfig, kbNoKey, hcNoContext );
    if (toolCount > 0)
        run + newLine() + toolMenuItems(toolCount) + newLine();
    run + *new TMenuItem( "Show/Hide ~O~utput", cmToggleOutput, kbNoKey, hcNoContext );

    return new TurboMenuBar( r,
        *new TSubMenu( "~F~ile", kbAltF, hcNoContext ) +
            *new TMenuItem( "~N~ew", cmNew, kbCtrlN, hcNoContext, "Ctrl-N" ) +
            *new TMenuItem( "New ~F~ile...", cmNewNamedFile, kbNoKey, hcNoContext ) +
            *new TMenuItem( "New Sp~e~c...", cmNewSpec, kbNoKey, hcNoContext ) +
            *new TMenuItem( "~O~pen...", cmOpen, kbCtrlO, hcNoContext, "Ctrl-O" ) +
            *new TMenuItem( "Open ~D~irectory...", cmOpenDir, kbNoKey, hcNoContext ) +
            *new TMenuItem( "Go to ~A~nything...", cmGotoAnything, kbNoKey, hcNoContext, "Ctrl-P" ) +
            *new TMenuItem( "Command Pa~l~ette...", cmCommandPalette, kbNoKey, hcNoContext, "Ctrl-B" ) +
            newLine() +
            *new TMenuItem( "~S~ave", cmSave, kbCtrlS, hcNoContext, "Ctrl-S" ) +
            *new TMenuItem( "S~a~ve As...", cmSaveAs, kbNoKey, hcNoContext ) +
            *new TMenuItem( "~R~ename...", cmRename, kbF2, hcNoContext, "F2" ) +
            newLine() +
            *new TMenuItem( "~C~lose", cmCloseEditor, kbCtrlW, hcNoContext, "Ctrl-W" ) +
            *new TMenuItem( "Close All", cmCloseAll, kbNoKey, hcNoContext ) +
            *new TMenuItem( "Close ~P~roject", cmCloseProject, kbNoKey, hcNoContext ) +
            newLine() +
            *new TMenuItem( "New ~T~erminal", cmNewTerminal, kbNoKey, hcNoContext ) +
            newLine() +
            *new TMenuItem( "S~u~spend", cmDosShell, kbNoKey, hcNoContext ) +
            *new TMenuItem( "E~x~it", cmQuit, kbCtrlQ, hcNoContext, "Ctrl-Q" ) +
        *new TSubMenu( "~E~dit", kbAltE ) +
            *new TMenuItem( "~U~ndo", cmUndo, kbCtrlZ, hcNoContext, "Ctrl-Z" ) +
            *new TMenuItem( "Re~d~o", cmRedo, kbCtrlY, hcNoContext, "Ctrl-Y" ) +
            newLine() +
            *new TMenuItem( "Cu~t~", cmCut, kbCtrlX, hcNoContext, "Ctrl-X" ) +
            *new TMenuItem( "~C~opy", cmCopy, kbCtrlC, hcNoContext, "Ctrl-C" ) +
            *new TMenuItem( "~P~aste", cmPaste, kbCtrlV, hcNoContext, "Ctrl-V" ) +
            newLine() +
            *new TMenuItem( "~F~ind...", cmFind, kbCtrlF, hcNoContext, "Ctrl-F" ) +
            *new TMenuItem( "~R~eplace...",cmReplace, kbCtrlR, hcNoContext, "Ctrl-R" ) +
            *new TMenuItem( "~G~o to Line...",cmGoToLine, kbCtrlG, hcNoContext, "Ctrl-G" ) +
            *new TMenuItem( "Find ~N~ext", cmSearchAgain, kbF3, hcNoContext, "F3" ) +
            *new TMenuItem( "Find ~P~revious", cmSearchPrev, kbShiftF3, hcNoContext, "Shift-F3" ) +
            newLine() +
            *new TMenuItem( "C~o~mplete", cmCompletion, kbNoKey, hcNoContext, "Ctrl-Space" ) +
        *new TSubMenu( "~V~iew", kbAltV ) +
            *new TMenuItem( "Line ~N~umbers", cmToggleLineNums, kbF8, hcNoContext, "F8" ) +
            *new TMenuItem( "Line ~W~rapping", cmToggleWrap, kbF9, hcNoContext, "F9" ) +
            *new TMenuItem( "Auto ~I~ndent", cmToggleIndent, kbNoKey, hcNoContext ) +
            *new TMenuItem( "~H~idden Files", cmToggleHidden, kbNoKey, hcNoContext ) +
            *new TMenuItem( "Chan~g~e History", cmToggleChangeHistory, kbNoKey, hcNoContext ) +
            *new TMenuItem( "Long Line G~u~ide", cmToggleEdge, kbNoKey, hcNoContext ) +
            newLine() +
            *new TMenuItem( "Code ~F~olding", cmToggleFolding, kbNoKey, hcNoContext ) +
            *new TMenuItem( "Fold a~t~ Cursor", cmFoldAtCursor, kbNoKey, hcNoContext ) +
            *new TMenuItem( "Fold ~A~ll", cmFoldAll, kbNoKey, hcNoContext ) +
            *new TMenuItem( "~U~nfold All", cmUnfoldAll, kbNoKey, hcNoContext ) +
            newLine() +
            *new TMenuItem( "Toggle ~B~ookmark", cmToggleBookmark, kbNoKey, hcNoContext ) +
            *new TMenuItem( "~N~ext Bookmark", cmNextBookmark, kbNoKey, hcNoContext ) +
            *new TMenuItem( "~P~revious Bookmark", cmPrevBookmark, kbNoKey, hcNoContext ) +
        *new TSubMenu( "~C~ode", kbAltC ) +
            *new TMenuItem( "~T~oggle Comment", cmToggleComment, kbCtrlE, hcNoContext, "Ctrl-E" ) +
            newLine() +
            *new TMenuItem( "~U~ppercase", cmSelUppercase, kbNoKey, hcNoContext ) +
            *new TMenuItem( "~L~owercase", cmSelLowercase, kbNoKey, hcNoContext ) +
            *new TMenuItem( "~C~apitalize", cmSelCapitalize, kbNoKey, hcNoContext ) +
            newLine() +
            *new TMenuItem( "Select ~N~ext Occurrence", cmSelectNextOccurrence, kbCtrlD, hcNoContext, "Ctrl-D" ) +
            *new TMenuItem( "Select ~A~ll Occurrences", cmSelectAllOccurrences, kbNoKey, hcNoContext ) +
            *new TMenuItem( "S~k~ip Occurrence", cmSkipOccurrence, kbNoKey, hcNoContext ) +
            *new TMenuItem( "Undo Last Selecti~o~n", cmUndoSelection, kbNoKey, hcNoContext, "Ctrl-U" ) +
            newLine() +
            *new TMenuItem( "Add Caret ~U~p", cmAddCaretUp, kbNoKey, hcNoContext, "Ctrl-Alt-Up" ) +
            *new TMenuItem( "Add Caret ~D~own", cmAddCaretDown, kbNoKey, hcNoContext, "Ctrl-Alt-Down" ) +
            *new TMenuItem( "Spl~i~t into Lines", cmSplitSelectionLines, kbNoKey, hcNoContext ) +
        *new TSubMenu( "~G~it", kbAltG ) +
            *new TMenuItem( "~C~ommit...", cmGitCommit, kbNoKey, hcNoContext ) +
            newLine() +
            *new TMenuItem( "~F~etch", cmGitFetch, kbNoKey, hcNoContext ) +
            *new TMenuItem( "Pu~l~l", cmGitPull, kbNoKey, hcNoContext ) +
            *new TMenuItem( "~P~ush", cmGitPush, kbNoKey, hcNoContext ) +
            newLine() +
            *new TMenuItem( "Ne~w~ Branch...", cmGitNewBranch, kbNoKey, hcNoContext ) +
            *new TMenuItem( "~M~erge Branch...", cmGitMerge, kbNoKey, hcNoContext ) +
            *new TMenuItem( "~A~bort Merge", cmGitMergeAbort, kbNoKey, hcNoContext ) +
            *new TMenuItem( "Co~n~tinue Merge", cmGitMergeContinue, kbNoKey, hcNoContext ) +
            newLine() +
            *new TMenuItem( "~R~efresh Status", cmGitRefresh, kbNoKey, hcNoContext ) +
        run +
        *new TSubMenu( "~D~ebug", kbAltD ) +
            *new TMenuItem( "~S~tart Debugging", cmDebugStart, kbNoKey, hcNoContext ) +
            *new TMenuItem( "St~o~p", cmDebugStop, kbNoKey, hcNoContext ) +
            newLine() +
            *new TMenuItem( "~C~ontinue", cmDebugContinue, kbNoKey, hcNoContext ) +
            *new TMenuItem( "Step O~v~er", cmDebugStepOver, kbNoKey, hcNoContext ) +
            *new TMenuItem( "Step ~I~nto", cmDebugStepInto, kbNoKey, hcNoContext ) +
            *new TMenuItem( "Step Ou~t~", cmDebugStepOut, kbNoKey, hcNoContext ) +
            newLine() +
            *new TMenuItem( "Call Stac~k~", cmDebugCallStack, kbNoKey, hcNoContext ) +
            *new TMenuItem( "~V~ariables", cmDebugVariables, kbNoKey, hcNoContext ) +
            *new TMenuItem( "Toggle ~B~reakpoint", cmToggleBreakpoint, kbNoKey, hcNoContext ) +
        *new TSubMenu( "L~u~a", kbAltU ) +
            *new TMenuItem( "~R~un Script...", cmLuaRunScript, kbNoKey, hcNoContext ) +
            *new TMenuItem( "~N~ew Script...", cmLuaNewScript, kbNoKey, hcNoContext ) +
            newLine() +
            *new TMenuItem( "Re~l~oad Config", cmLuaReload, kbNoKey, hcNoContext ) +
        *new TSubMenu( "~A~gent", kbAltA ) +
            *new TMenuItem( "~T~oggle Agent", cmToggleAgent, kbAlt0, hcNoContext, "Alt-0" ) +
            *new TMenuItem( "~R~estart Agent", cmRestartAgent, kbNoKey, hcNoContext ) +
            newLine() +
            *new TMenuItem( "~S~elect Agent...", cmSelectAgent, kbNoKey, hcNoContext ) +
        windows +
        *new TSubMenu( "~S~ettings", kbAltS ) +
            *new TMenuItem( "~C~olour Scheme...", cmThemeSettings, kbNoKey, hcNoContext ) +
            newLine() +
            *new TMenuItem( "Colour Mode: A~u~to", cmColorModeAuto, kbNoKey, hcNoContext ) +
            *new TMenuItem( "Colour Mode: ~F~ull (24-bit)", cmColorModeFull, kbNoKey, hcNoContext ) +
            *new TMenuItem( "Colour Mode: 1~6~-colour (classic)", cmColorMode16, kbNoKey, hcNoContext ) +
            newLine() +
            *new TMenuItem( "Tree Icons: Au~t~o", cmTreeIconsAuto, kbNoKey, hcNoContext ) +
            *new TMenuItem( "Tree Icons: ~N~erd Font", cmTreeIconsNerd, kbNoKey, hcNoContext ) +
            *new TMenuItem( "Tree Icons: Uni~c~ode", cmTreeIconsUnicode, kbNoKey, hcNoContext ) +
            *new TMenuItem( "Tree Icons: ASC~I~I", cmTreeIconsAscii, kbNoKey, hcNoContext ) +
            newLine() +
            *new TMenuItem( "~L~anguage Servers...", cmLspSettings, kbNoKey, hcNoContext ) +
            *new TMenuItem( "~D~ebuggers...", cmDebugSettings, kbNoKey, hcNoContext ) +
            newLine() +
            *new TMenuItem( "~A~uto-save on Focus Loss", cmToggleAutoSave, kbNoKey, hcNoContext ) +
        *new TSubMenu( "~H~elp", kbAltH ) +
            *new TMenuItem( "~K~eyboard shortcuts", cmHelp, kbF1, hcNoContext, "F1" ) +
            newLine() +
            *new TMenuItem( "~A~bout...", cmAbout, kbNoKey, hcNoContext )
            );

}

TStatusLine *TurboApp::initStatusLine( TRect r )
{
    r.a.y = r.b.y-1;
    return new TStatusLine( r,
        *new TStatusDef( 0, 0xFFFF ) +
            *new TStatusItem( 0, kbAltX, cmQuit ) +
            *new TStatusItem( "~Ctrl-N~ New", kbNoKey, cmNew ) +
            *new TStatusItem( "~Ctrl-O~ Open", kbNoKey, cmOpen ) +
            *new TStatusItem( "~Ctrl-S~ Save", kbNoKey, cmSave ) +
            *new TStatusItem( "~F6~ Next", kbF6, cmEditorNext ) +
            *new TStatusItem( "~F12~ Menu", kbF12, cmMenu ) +
            *new TStatusItem( 0, TKey(kbCtrlZ, kbShift), cmRedo ) +
            *new TStatusItem( 0, kbCtrlX, cmCut ) +
            *new TStatusItem( 0, kbCtrlC, cmCopy ) +
            *new TStatusItem( 0, kbCtrlV, cmPaste ) +
            *new TStatusItem( 0, kbShiftDel, cmCut ) +
            *new TStatusItem( 0, kbCtrlIns, cmCopy ) +
            *new TStatusItem( 0, kbShiftIns, cmPaste ) +
            *new TStatusItem( 0, kbCtrlTab, cmEditorNext ) +
            *new TStatusItem( 0, kbAltTab, cmEditorNext ) +
            *new TStatusItem( 0, kbShiftF6, cmEditorPrev ) +
            *new TStatusItem( 0, TKey(kbCtrlTab, kbShift), cmEditorPrev ) +
            *new TStatusItem( 0, TKey(kbAltTab, kbShift), cmEditorPrev ) +
            *new TStatusItem( 0, TKey('/', kbCtrlShift), cmToggleComment ) +
            *new TStatusItem( 0, TKey('_', kbCtrlShift), cmToggleComment ) +
            *new TStatusItem( 0, kbF5, cmZoom ) +
            *new TStatusItem( 0, kbCtrlF5, cmResize ) +
            // Fuzzy navigation. Bound on the status line so they convert to a
            // command inside getEvent before the focused editor (Scintilla) sees
            // the key. Ctrl-P (Goto Anything) and Ctrl-B (Command Palette) are
            // both free single-Ctrl combos every terminal can send -- Ctrl-Shift-P
            // is avoided because terminals can't distinguish it from Ctrl-P.
            *new TStatusItem( 0, kbCtrlP, cmGotoAnything ) +
            *new TStatusItem( 0, kbCtrlB, cmCommandPalette ) +
            // Alt-0 opens/focuses the coding-agent window (Alt-1..9 are reserved
            // by tvision for window selection; Alt-0 is free and terminal-safe).
            *new TStatusItem( 0, kbAlt0, cmToggleAgent ) +
            // Undo-selection on Ctrl+U (only fires with an editor focused, as the
            // command is disabled otherwise; converted before the editor sees the
            // key, like the navigation overlays above). Split-into-lines is NOT
            // bound to a key: in a terminal Ctrl+Shift+L is indistinguishable from
            // Ctrl+L (Cut current line), so binding it would shadow that. Reach
            // Split-into-lines from the Selection menu or the Command Palette.
            *new TStatusItem( 0, kbCtrlU, cmUndoSelection )
            );
}

TurboApp::~TurboApp() = default;

void TurboApp::shutDown()
{
    // Persist the open windows so the next open of this project restores them
    // (no-op when no project is open). Editors are still valid at this point.
    saveSession();
    if (mcp)
        mcp->stop(); // join the socket threads before luaMgr/editors tear down
    if (lsp)
        lsp->shutdown();
    if (dap)
        dap->shutdown();
    if (git)
        git->shutdown();
    if (watcher)
        watcher->stop();
    // Not stopped by closeProject: the global skills home isn't project-scoped.
    if (skillsWatcher)
        skillsWatcher->stop();
    pendingAfterBuild = nullptr;
    if (buildRunner)
        buildRunner->stop();
    stopAllTools();
    docTree = nullptr;
    clock = nullptr;
    branchView = nullptr;
    outputWin = nullptr;
    TApplication::shutDown();
}

void TurboApp::idle()
{
    TApplication::idle();
    if (clock)
        clock->update();
    if (lsp)
        lsp->pump();
    if (dap)
        dap->pump();
    if (mcp)
        mcp->pump();
    if (watcher)
        onFilesChanged();
    if (git)
    {
        git->pump(docTree);
        updateEditorConflictBars();
    }
    // Drain any output the terminal child processes produced since the last tick
    // and repaint the affected views (the reader threads wake the loop for us).
    for (auto *t : terminals)
        t->pump();
    // Stream build-command output into the output pane (same reader-thread +
    // wakeUp pattern as the terminals).
    if (buildRunner)
        buildRunner->pump();
    // Drain each running tool process's output into its Output tab.
    for (auto &t : tools)
        if (t.runner)
            t.runner->pump();
    // Run any action deferred from a runner's onExit (e.g. start Run after a
    // build-first), now that we're safely outside that runner's pump.
    if (pendingAfterBuild)
    {
        auto action = std::move(pendingAfterBuild);
        pendingAfterBuild = nullptr;
        action();
    }
    // Keep menu check marks in sync with per-editor toggle state and the active
    // editor. Cheap: only rewrites a label when its checked state changes.
    refreshMenuChecks();
    refreshWindowList();
    refreshBranchView();
}

void TurboApp::getEvent(TEvent &event)
{
    if (!argsParsed) {
        argsParsed = true;
        // No project is opened automatically any more: the user opens one with
        // `turbo <dir>` (e.g. `turbo .`) or File > Open Directory. Bring up the
        // Lua interpreter so the global config/scripts work with no project, and
        // surface those global scripts in the otherwise-empty tree.
        initLua();
        refreshLuaScriptsInTree();
        refreshSkillsInTree();
        parseArgs();
    }
    TApplication::getEvent(event);
}

void TurboApp::handleEvent(TEvent &event)
{
    TApplication::handleEvent(event);
    bool handled = false;
    if (event.what == evCommand) {
        handled = true;
        switch (event.message.command) {
            case cmNew: fileNew(); break;
            case cmNewNamedFile: fileNewNamedFile(); break;
            case cmOpen: fileOpen(); break;
            case cmOpenDir: fileOpenDir(); break;
            case cmCloseProject: closeProject(); break;
            case cmGotoAnything: gotoAnything(); break;
            case cmCommandPalette: commandPalette(); break;
            case cmBuild: runBuild(); break;
            case cmRun: runRun(); break;
            case cmTest: runTest(); break;
            case cmStop: stopAll(); break;
            case cmBuildConfig: editBuildConfig(); break;
            case cmToolsConfig: editToolsConfig(); break;
            case cmToggleOutput: toggleOutputView(); break;
            case cmDebugStart: startDebug(); break;
            case cmDebugStop: stopDebug(); break;
            case cmDebugContinue: if (dap) dap->continueExec(); break;
            case cmDebugStepOver: if (dap) dap->stepOver(); break;
            case cmDebugStepInto: if (dap) dap->stepIn(); break;
            case cmDebugStepOut: if (dap) dap->stepOut(); break;
            case cmDebugCallStack:
                if (outputWin && dap && dap->sessionActive())
                {
                    showOutput();
                    outputWin->showExtraTab(otCallStack);
                    outputWin->focus(); // so the panel takes keyboard input
                }
                break;
            case cmDebugVariables:
                if (outputWin && dap && dap->sessionActive())
                {
                    showOutput();
                    outputWin->showExtraTab(otVariables);
                    outputWin->focus();
                }
                break;
            case cmLuaRunScript: runLuaScriptPicker(); break;
            case cmLuaNewScript: luaNewScript(); break;
            case cmLuaReload: reloadLuaConfig(); break;
            case cmEditorNext:
            case cmEditorPrev:
                showEditorList(&event);
                break;
            case cmCloseAll: closeAll(); break;
            case cmToggleTree: toggleTreeView(); break;
            case cmNewTerminal: newTerminal(); break;
            case cmToggleAgent: toggleAgent(); break;
            case cmSelectAgent: selectAgent(); break;
            case cmRestartAgent: restartAgent(); break;
            case cmToggleHidden: toggleHiddenFiles(); break;
            case cmToggleAutoSave: toggleAutoSave(); break;
            case cmLspSettings: editLspSettings(); break;
            case cmDebugSettings: editDebugSettings(); break;
            case cmNewSpec: newSpec(); break;
            case cmThemeSettings: editThemeSettings(); break;
            case cmApplyTheme: applyActiveTheme(); break;
            case cmColorModeAuto: setColorMode("auto"); break;
            case cmColorModeFull: setColorMode("full"); break;
            case cmColorMode16:   setColorMode("16");   break;
            case cmTreeIconsAuto:    setTreeIcons("auto");    break;
            case cmTreeIconsNerd:    setTreeIcons("nerd");    break;
            case cmTreeIconsUnicode: setTreeIcons("unicode"); break;
            case cmTreeIconsAscii:   setTreeIcons("ascii");   break;
            case cmGitRefresh: gitRefresh(); break;
            case cmGitCommit: gitCommitDialog(); break;
            case cmGitFetch: gitRemote(0); break;
            case cmGitPull: gitRemote(1); break;
            case cmGitPush: gitRemote(2); break;
            case cmGitNewBranch: gitNewBranch(); break;
            case cmGitMerge: gitMerge(); break;
            case cmGitMergeAbort: gitMergeAbort(); break;
            case cmGitMergeContinue: gitMergeContinue(); break;
            case cmGitResolveFile:
                if (auto *w = (EditorWindow *) event.message.infoPtr)
                    gitResolveFile(w);
                break;
            case cmShowCompletion:
                if (lsp)
                    if (auto *w = (EditorWindow *) event.message.infoPtr)
                        lsp->showCompletion(*w);
                break;
            case cmRevealInTree:
                if (docTree) {
                    if (!(docTree->state & sfVisible))
                        toggleTreeView();
                    if (auto *w = (EditorWindow *) event.message.infoPtr)
                        docTree->tree->revealEditor(w);
                }
                break;
            case cmTreeNext:
                if (docTree)
                    docTree->tree->focusNext();
                break;
            case cmTreePrev:
                if (docTree)
                    docTree->tree->focusPrev();
                break;
            case cmAbout:
                TurboHelp::executeAboutDialog(*deskTop);
                break;
            case cmHelp:
                TurboHelp::showOrFocusHelpWindow(*deskTop);
                break;
            default:
                if (event.message.command >= cmWindowBase &&
                    event.message.command < cmWindowBase + windowListMax)
                    focusRecentWindow(event.message.command - cmWindowBase);
                else if (event.message.command >= cmToolBase &&
                         event.message.command < cmToolBase + toolListMax)
                    toggleTool(event.message.command - cmToolBase);
                else if (event.message.command >= cmLuaScriptBase &&
                         event.message.command < cmLuaScriptBase + luaScriptListMax)
                    runDiscoveredLuaScript(event.message.command - cmLuaScriptBase);
                else if (event.message.command >= cmLuaCommandBase &&
                         event.message.command < cmLuaCommandBase + luaCommandListMax)
                {
                    if (luaMgr)
                        luaMgr->runRegisteredCommand(event.message.command - cmLuaCommandBase);
                }
                else
                    handled = false;
                break;
        }
    }
    if (handled)
        clearEvent(event);
}

void TurboApp::parseArgs()
{
    if (!(argc && argv))
        return;
    // A directory argument opens that directory as the project (so `turbo .`
    // opens the current directory); the first one wins, since only one project
    // runs at a time. Everything else is treated as a file to open.
    std::vector<const char *> files;
    bool projectOpened = false;
    for (int i = 1; i < argc; ++i) {
        std::error_code ec;
        if (std::filesystem::is_directory(argv[i], ec)) {
            if (!projectOpened) {
                openProject(argv[i]);
                projectOpened = true;
            }
            // Extra directory args are ignored: one project at a time.
        } else {
            files.push_back(argv[i]);
        }
    }
    if (files.empty())
        return;
    auto *w = new TWindow(TRect(15, 8, 65, 19), "Please Wait...", wnNoNumber);
    w->flags = 0;
    w->options |= ofCentered;
    w->palette = wpGrayWindow;
    w->insert( new TStaticText(TRect(2, 2, 48, 3), "Opening file:"));
    auto *current = new TParamText(TRect(2, 3, 48, 9));
    w->insert(current);
    insert(w);
    for (const char *path : files) {
        current->setText("%s", path);
        TScreen::flushScreen();
        fileOpenOrNew(path);
    }
    remove(w);
    TObject::destroy(w);
}

void TurboApp::fileNew()
{
    addEditor(createScintilla(), "");
}

void TurboApp::fileNewNamedFile()
{
    // Guided new file: open a scratch buffer, then immediately run Save As so the
    // name and location are chosen up front. Setting the path detects the
    // language, so the correct lexer (and line numbers) apply from the first
    // keystroke -- unlike plain cmNew, which leaves an unnamed, unlexed buffer.
    // If the user cancels the dialog, discard the throwaway buffer so "New
    // File..." leaves nothing behind.
    addEditor(createScintilla(), "");
    EditorWindow *w = MRUlist.empty() ? nullptr : MRUlist.next->self;
    if (!w)
        return;
    TurboFileDialogs dlgs {*this};
    if (w->getEditor().saveAs(dlgs))
    {
        // The new file now exists on disk; the filesystem watcher adds it to the
        // tree. Refresh git so its status badge appears without delay.
        if (git)
            git->requestStatus();
    }
    else
        message(w, evCommand, cmClose, 0);
}

void TurboApp::fileOpen()
{
    TurboFileDialogs dlgs {*this};
    turbo::openFile([&] () -> auto& {
        return createScintilla();
    }, [&] (auto &scintilla, auto *path) {
        addEditor(scintilla, path);
    }, dlgs);
}

void TurboApp::fileOpenOrNew(const char *path)
{
    char abspath[MAXPATH];
    strnzcpy(abspath, path, MAXPATH);
    fexpand(abspath);
    auto &scintilla = createScintilla();
    if (turbo::readFile(scintilla, abspath, turbo::acceptMissingFilesOnOpen))
        addEditor(scintilla, abspath);
}

void TurboApp::openFileFromTree(const char *absPath)
{
    fileOpenOrNew(absPath);
}

void TurboApp::openOrFocus(const std::string &absPath, long line) noexcept
{
    if (absPath.empty())
        return;
    EditorWindow *found = nullptr;
    MRUlist.forEach([&] (EditorWindow *w) {
        if (w && w->filePath() == absPath)
            found = w;
    });
    if (found)
        found->focus();
    else
    {
        fileOpenOrNew(absPath.c_str());
        found = MRUlist.empty() ? nullptr : MRUlist.next->self;
    }
    if (found && line >= 0)
    {
        auto &ed = found->getEditor();
        ed.callScintilla(SCI_GOTOLINE, line, 0U);
        ed.callScintilla(SCI_SCROLLCARET, 0U, 0U);
        ed.redraw();
    }
    frecency.record(absPath);
}

void TurboApp::gotoAnything()
{
    runGotoAnything(*this);
}

void TurboApp::commandPalette()
{
    // Offer every discovered Lua script as a palette entry ("Lua Script: <name>")
    // alongside the static commands; selecting one dispatches cmLuaScriptBase + i.
    std::vector<PaletteExtra> extra;
    std::vector<std::string> scripts = discoverLuaScripts();
    int n = std::min((int) scripts.size(), (int) luaScriptListMax);
    for (int i = 0; i < n; ++i)
    {
        bool isProject = !projectRoot.empty() && scripts[i].rfind(projectRoot, 0) == 0;
        std::string name = std::filesystem::path(scripts[i]).filename().string();
        extra.push_back({ "Lua Script: " + name, isProject ? "project" : "global",
                          (ushort) (cmLuaScriptBase + i) });
    }
    // Commands registered from Lua (turbo.register_command) appear too.
    if (luaMgr)
    {
        int nc = std::min(luaMgr->commandCount(), (int) luaCommandListMax);
        for (int i = 0; i < nc; ++i)
            extra.push_back({ luaMgr->commandName(i), luaMgr->commandDescription(i),
                              (ushort) (cmLuaCommandBase + i) });
    }
    ushort cmd = runCommandPalette(!MRUlist.empty(), extra);
    if (cmd)
    {
        // Dispatch through the normal event path so the command reaches the
        // focused editor/app exactly like a menu pick.
        TEvent ev;
        ev.what = evCommand;
        ev.message.command = cmd;
        ev.message.infoPtr = nullptr;
        putEvent(ev);
    }
}

// The two Lua homes, mirroring the two Skills homes (.claude/skills, ~/.claude/
// skills). Project Lua is committed with the repository: <projectRoot>/turbo-scripts
// holds init.lua at its top and the runnable scripts beside it. Global Lua is the
// user's own, shared across every project: ~/.turbo holds init.lua, with the
// runnable scripts under ~/.turbo/scripts.
static std::string projectLuaHome(const std::string &projectRoot)
{
    return projectRoot.empty() ? std::string() : projectRoot + "/turbo-scripts";
}

static std::string globalLuaHome()
{
    if (const char *home = ::getenv("HOME"))
        return std::string(home) + "/.turbo";
    return {};
}

// Where a home keeps its runnable scripts: beside init.lua in the project home,
// under scripts/ in the global one.
static std::string globalLuaScriptsDir()
{
    std::string home = globalLuaHome();
    return home.empty() ? home : home + "/scripts";
}

// Treat <projectRoot>/.turbo as a disposable, per-user cache: ensure it carries a
// .gitignore that ignores its entire contents (session, socket, config.json), so
// none of it gets committed -- while everything meant to be shared with the repo
// (turbo-scripts/) lives outside .turbo. Non-destructive: writes only when .turbo
// already exists and has no .gitignore yet, so it never clobbers a user's own.
static void ensureTurboCacheIgnored(const std::string &projectRoot) noexcept
{
    if (projectRoot.empty())
        return;
    std::error_code ec;
    std::string turbo = projectRoot + "/.turbo";
    if (!std::filesystem::is_directory(turbo, ec))
        return;
    std::string gi = turbo + "/.gitignore";
    if (std::filesystem::exists(gi, ec))
        return;
    std::ofstream f(gi, std::ios::out);
    if (f)
        f << "*\n";
}

void TurboApp::openProject(const std::string &dir) noexcept
{
    if (!docTree)
        return;
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(dir, ec);
    if (ec)
    {
        messageBox(mfError | mfOKButton, "Cannot open '%s' as a project.", dir.c_str());
        return;
    }
    std::string root = abs.lexically_normal().string();
    // Drop a trailing separator so the root compares equal to a child entry's
    // dirname() (the file tree relies on that, e.g. addNode's `dir == rootPath`).
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\'))
        root.pop_back();

    // One project at a time: close any current one first (clears the tree, the
    // watcher, git/LSP state) so the fresh scan starts from a clean slate.
    if (!projectRoot.empty())
        closeProject();

    projectRoot = root; // build/run commands run from here
    // Run as if launched from the project directory, so the integrated terminal,
    // the Lua shell helper and any relative paths resolve against it -- matching
    // the old `cd dir && turbo` behaviour.
    std::filesystem::current_path(root, ec);

    buildConfig.load(projectRoot); // .turbo/config.json (no-op if absent)
    debugConfig.load(projectRoot); // .turbo/debug.json (no-op if absent)
    if (dap)
        dap->setConfig(debugConfig);
    applyToolConfig();             // build the Run menu's tool toggles from it
    ensureTurboCacheIgnored(projectRoot); // retrofit .gitignore on existing .turbo
    docTree->tree->setShowHidden(settings.showHidden); // before the first scan
    docTree->tree->scanDirectory(root.c_str());
    // Re-link any editors already open onto their freshly created tree nodes, so
    // open files show bold / get git badges even when opened before the project.
    MRUlist.forEach([&] (EditorWindow *w) {
        if (w)
            docTree->tree->linkEditor(w);
    });
    if (lsp)
        lsp->setRootPath(root.c_str());
    if (dap)
        dap->setRootPath(root.c_str());
    if (git)
        git->setWorkspace(root.c_str());
    if (watcher)
        watcher->start(root.c_str());
    // Load the project's turbo-scripts/init.lua hooks. luaMgr already exists (created
    // at startup with the global config); re-running resets the handler registry and
    // loads project-then-global init.lua, so project hooks take effect.
    if (luaMgr)
        luaMgr->loadInitScripts(projectLuaHome(projectRoot), globalLuaHome());
    // Start the MCP server for this project and point the agent at it. The Lua
    // tools are (re)registered by loadInitScripts just above, so tools/list is
    // ready before any agent connects.
    if (mcp)
    {
        std::string sock = mcpSocketPath(projectRoot);
        if (mcp->start(sock))
        {
            // .mcp.json makes the server discoverable to any agent; for Claude
            // Code we also register it at local scope (auto-approved, no trust
            // prompt), which shadows the .mcp.json entry.
            writeAgentMcpConfig(projectRoot, sock);
            if (resolveAgentCommand(buildConfig.agent, settings.defaultAgent)
                    .find("claude") != std::string::npos)
                registerClaudeMcpServerAsync(projectRoot, sock);
        }
    }
    refreshLuaScriptsInTree(); // now includes the project's own scripts
    refreshSkillsInTree();     // and the project's .claude/skills
    // Opening a project is an explicit "work on this" action, so surface the file
    // tree straight away -- even on smaller terminals, where it starts hidden.
    // Done before restoreSession so the restored editors lay out beside the tree
    // (showing it afterwards would re-shift their already-placed bounds).
    if (!(docTree->state & sfVisible))
        toggleTreeView();
    restoreSession(); // reopen the windows that were open last time, if any
    refreshMenuChecks();
}

void TurboApp::closeProject() noexcept
{
    if (projectRoot.empty())
        return; // nothing open
    saveSession();         // remember the open windows for the next open
    closeProjectEditors(); // close editors holding files inside the project
    if (mcp)
        mcp->stop();       // drop the project's MCP server (restarts on next open)
    projectRoot.clear();
    stopAllTools();               // kill the project's tool processes
    buildConfig = BuildConfig {}; // forget the project's build/run config
    applyToolConfig();            // clears 'tools' + removes their Output tabs/menu
    lastBuildCommand.clear();
    if (watcher)
        watcher->stop();
    if (git)
        git->clearWorkspace(); // clears badges + branch indicator via pump()
    if (docTree)
    {
        docTree->tree->clear();      // empty the tree (keeps the Lua section)
        docTree->setBranchInfo("");  // drop the branch from the tree title now
    }
    refreshLuaScriptsInTree(); // with no project, show the global scripts only
    refreshSkillsInTree();     // ... and only the global skills
    refreshMenuChecks();
}

void TurboApp::fileOpenDir()
{
    // TChDirDialog is a folder chooser; pressing its OK button chdir()s the
    // process to the selected directory (in TChDirDialog::valid), so the chosen
    // path is simply the new working directory.
    auto *d = new TChDirDialog(cdNormal, 0);
    ushort res = deskTop->execView(d);
    TObject::destroy(d);
    if (res == cmCancel)
        return;
    std::error_code ec;
    std::string dir = std::filesystem::current_path(ec).string();
    if (!ec && !dir.empty())
        openProject(dir);
}

// The path of 'p' relative to project root 'root' if 'p' lives inside it, else
// an empty string. Used both to decide whether an editor belongs to the project
// and to compute the path stored in the session file.
static std::string relInProject(const std::string &root, const std::string &p) noexcept
{
    if (root.empty() || p.empty())
        return {};
    std::error_code ec;
    std::filesystem::path rel = std::filesystem::path(p).lexically_relative(root);
    std::string s = rel.generic_string();
    if (s.empty() || s == "." || s == ".." || s.rfind("../", 0) == 0)
        return {}; // not inside the project root
    return s;
}

void TurboApp::closeProjectEditors() noexcept
{
    if (projectRoot.empty())
        return;
    // Collect the in-project editors up front: closing one mutates the MRU list,
    // but the other window pointers stay valid until we close them.
    std::vector<EditorWindow *> victims;
    MRUlist.forEach([&] (EditorWindow *w) {
        if (w && !relInProject(projectRoot, w->filePath()).empty())
            victims.push_back(w);
    });
    for (auto *w : victims)
    {
        // Standard window close: prompts to save a dirty buffer. If the user
        // cancels, the window simply stays open (we move on to the next).
        message(w, evCommand, cmClose, 0);
        TScreen::flushScreen();
    }
}

// The session file stores one line per in-project editor, in MRU order, so
// reopening a project puts every window back exactly where it was. Fields are
// tab-separated with the (possibly space-containing) relative path last:
//   ax ay bx by  zax zay zbx zby  firstVisibleLine  anchorPos caretPos  active  relpath
// ax,ay,bx,by are the window's desktop-relative bounds; zax..zby is its zoomRect
// (the un-zoomed bounds Turbo Vision restores on the next F5, so a window saved
// while zoomed comes back zoomed AND un-zooms to the right size); firstVisibleLine
// is the scroll position; anchor/caret restore the selection; active flags the
// window that had focus. SESSION_FIELDS counts the numeric columns before the path.
enum { SESSION_FIELDS = 12 };

void TurboApp::saveSession() noexcept
{
    if (projectRoot.empty())
        return;
    std::string activeRel;
    if (auto *f = focusedEditor())
        activeRel = relInProject(projectRoot, f->filePath());
    std::vector<std::string> lines;
    MRUlist.forEach([&] (EditorWindow *w) {
        if (!w)
            return;
        std::string rel = relInProject(projectRoot, w->filePath());
        if (rel.empty())
            return; // unsaved scratch buffer or a file outside the project
        TRect b = w->getBounds();
        TRect z = w->zoomRect; // un-zoomed bounds (preserves the zoom toggle)
        auto &ed = w->getEditor();
        long anchor = (long) ed.callScintilla(SCI_GETANCHOR, 0U, 0U);
        long caret = (long) ed.callScintilla(SCI_GETCURRENTPOS, 0U, 0U);
        long fvl = (long) ed.callScintilla(SCI_GETFIRSTVISIBLELINE, 0U, 0U);
        char buf[192];
        std::snprintf(buf, sizeof buf,
                      "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%ld\t%ld\t%ld\t%d\t",
                      b.a.x, b.a.y, b.b.x, b.b.y, z.a.x, z.a.y, z.b.x, z.b.y,
                      fvl, anchor, caret, rel == activeRel ? 1 : 0);
        lines.push_back(std::string(buf) + rel);
    });
    std::string path = projectRoot + "/.turbo/session";
    std::error_code ec;
    if (lines.empty())
    {
        std::filesystem::remove(path, ec); // no project windows: forget the session
        return;
    }
    std::filesystem::create_directories(projectRoot + "/.turbo", ec);
    ensureTurboCacheIgnored(projectRoot); // .turbo just (maybe) created: ignore it
    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (!f)
        return;
    for (const auto &l : lines)
        f << l << "\n";
}

void TurboApp::restoreSession() noexcept
{
    if (projectRoot.empty())
        return;
    std::ifstream f(projectRoot + "/.turbo/session");
    if (!f)
        return;
    struct Entry { long v[SESSION_FIELDS]; std::string rel; };
    std::vector<Entry> entries;
    std::string raw;
    while (std::getline(f, raw))
    {
        Entry e;
        size_t pos = 0;
        bool ok = true;
        for (int k = 0; k < SESSION_FIELDS; ++k)
        {
            size_t t = raw.find('\t', pos);
            if (t == std::string::npos) { ok = false; break; }
            e.v[k] = std::strtol(raw.c_str() + pos, nullptr, 10);
            pos = t + 1;
        }
        if (!ok)
            continue; // malformed / truncated line
        e.rel = raw.substr(pos);
        if (!e.rel.empty())
            entries.push_back(std::move(e));
    }
    // Open least-recently-used first so the most-recent ends up on top; then
    // bring the previously-active editor to the front. Files that no longer exist
    // are skipped, so a deleted file doesn't resurrect as an empty buffer.
    std::string activeAbs;
    for (auto it = entries.rbegin(); it != entries.rend(); ++it)
    {
        std::string abs = projectRoot + "/" + it->rel;
        std::error_code ec;
        if (!std::filesystem::exists(abs, ec))
            continue;
        // Open (or find) the window, then put it back where it was.
        EditorWindow *w = nullptr;
        MRUlist.forEach([&] (EditorWindow *e) {
            if (e && e->filePath() == abs) w = e;
        });
        if (!w)
        {
            fileOpenOrNew(abs.c_str());
            w = MRUlist.empty() ? nullptr : MRUlist.next->self;
        }
        if (!w)
            continue;
        TRect r {(int) it->v[0], (int) it->v[1], (int) it->v[2], (int) it->v[3]};
        w->locate(r); // clamps to the window's size limits / the current screen
        // Restore the zoom toggle: zoomRect is the bounds F5 will un-zoom to. A
        // window saved while zoomed has r == the maximised size, so it stays
        // zoomed and the next F5 collapses it back to this zoomRect.
        w->zoomRect = TRect((int) it->v[4], (int) it->v[5], (int) it->v[6], (int) it->v[7]);
        auto &ed = w->getEditor();
        ed.callScintilla(SCI_SETSEL, it->v[9], it->v[10]);       // anchor, caret
        ed.callScintilla(SCI_SETFIRSTVISIBLELINE, it->v[8], 0U); // scroll position
        ed.redraw();
        if (it->v[11])
            activeAbs = abs;
    }
    if (!activeAbs.empty())
        openOrFocus(activeAbs);
}

EditorWindow *TurboApp::focusedEditor() noexcept
{
    // handleFocus() keeps the focused window at the front of the MRU list.
    return MRUlist.empty() ? nullptr : MRUlist.next->self;
}

// Capture the full text of an editor via Scintilla.
static std::string luaEditorText(EditorWindow &w)
{
    auto &ed = w.getEditor();
    sptr_t len = ed.callScintilla(SCI_GETLENGTH, 0U, 0U);
    std::string s((size_t) len, '\0');
    if (len > 0)
        ed.callScintilla(SCI_GETTEXT, len + 1, (sptr_t) s.data());
    return s;
}

// Run a shell command in the app's working directory (== projectRoot) and
// return its stdout. Synchronous: scripts run on the UI thread for v1.
// popen/pclose are POSIX; MSVC spells them _popen/_pclose.
#ifdef _WIN32
#define TURBO_POPEN  ::_popen
#define TURBO_PCLOSE ::_pclose
#else
#define TURBO_POPEN  ::popen
#define TURBO_PCLOSE ::pclose
#endif
static std::string luaShellCapture(const std::string &cmd)
{
    std::string out;
    if (FILE *p = TURBO_POPEN(cmd.c_str(), "r"))
    {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, p)) > 0)
            out.append(buf, n);
        TURBO_PCLOSE(p);
    }
    return out;
}
#undef TURBO_POPEN
#undef TURBO_PCLOSE

void TurboApp::initLua() noexcept
{
    if (luaMgr)
        return; // already initialised

    LuaHost host;
    host.message = [] (const std::string &s) {
        messageBox(s.c_str(), mfInformation | mfOKButton);
    };
    host.activeFilePath = [this] () -> std::string {
        if (auto *w = focusedEditor())
            return w->filePath();
        return {};
    };
    host.activeFileText = [this] () -> std::string {
        if (auto *w = focusedEditor())
            return luaEditorText(*w);
        return {};
    };
    host.insertText = [this] (const std::string &t) {
        if (auto *w = focusedEditor())
        {
            auto &ed = w->getEditor();
            ed.callScintilla(SCI_REPLACESEL, 0U, (sptr_t) t.c_str());
            ed.redraw();
        }
    };
    host.openFile = [this] (const std::string &p) {
        openOrFocus(p);
    };
    host.saveFile = [this] () {
        if (auto *w = focusedEditor())
        {
            TEvent ev {};
            ev.what = evCommand;
            ev.message.command = cmSave;
            ev.message.infoPtr = nullptr;
            w->putEvent(ev);
        }
    };
    host.runCommand = [this] (int cmd) {
        TEvent ev {};
        ev.what = evCommand;
        ev.message.command = (ushort) cmd;
        ev.message.infoPtr = nullptr;
        putEvent(ev);
    };
    host.shell = [] (const std::string &c) {
        return luaShellCapture(c);
    };
    host.projectRoot = [this] () {
        return projectRoot;
    };

    luaMgr = std::make_unique<LuaManager>(std::move(host));

    // The MCP server exposes the same LuaHost hooks + registered commands to the
    // agent. It starts when a project opens (openProject), since its tools are
    // project-scoped. A socket message on the reader thread nudges the idle loop.
    mcp = std::make_unique<McpServer>(*luaMgr);
    mcp->setWake([] { TEventQueue::wakeUp(); });

    // init.lua lives at the top of each Lua home: project first, then global.
    luaMgr->loadInitScripts(projectLuaHome(projectRoot), globalLuaHome());
}

bool TurboApp::fireLuaEvent(const char *event) noexcept
{
    return !luaMgr || luaMgr->fireEvent(event);
}

bool TurboApp::fireLuaEvent(const char *event,
                            const std::vector<std::pair<std::string, std::string>> &params) noexcept
{
    return !luaMgr || luaMgr->fireEvent(event, params);
}

// Collect the runnable *.lua scripts directly inside 'dir' (non-recursive), sorted
// by name. init.lua is not one of them: it is the home's startup/hooks file, run by
// loadInitScripts, so it never shows up as a script to run.
static void scanLuaDir(const std::string &dir, std::vector<std::string> &out)
{
    if (dir.empty())
        return;
    std::error_code ec;
    size_t start = out.size();
    for (auto &e : std::filesystem::directory_iterator(dir, ec))
    {
        std::error_code fec;
        if (e.is_regular_file(fec) && e.path().extension() == ".lua" &&
            e.path().filename() != "init.lua")
            out.push_back(e.path().string());
    }
    std::sort(out.begin() + start, out.end());
}

// The .lua files listed under a Lua home in the tree: the home's own init.lua first
// (the hooks file, worth seeing at the top), then its runnable scripts.
static std::vector<std::string> luaHomeFiles(const std::string &home,
                                             const std::string &scriptsDir)
{
    std::vector<std::string> out;
    std::error_code ec;
    std::string init = home + "/init.lua";
    if (!home.empty() && std::filesystem::is_regular_file(init, ec))
        out.push_back(std::move(init));
    scanLuaDir(scriptsDir, out);
    return out;
}

std::vector<std::string> TurboApp::discoverLuaScripts() const noexcept
{
    std::vector<std::string> out;
    // Project scripts first, so the picker groups them ahead of the global ones.
    // The project home sits under projectRoot, so the picker's project-vs-global
    // split (rfind(projectRoot,0)==0) still holds.
    scanLuaDir(projectLuaHome(projectRoot), out);
    scanLuaDir(globalLuaScriptsDir(), out);
    return out;
}

void TurboApp::runLuaScriptPicker() noexcept
{
    std::vector<std::string> scripts = discoverLuaScripts();
    if (scripts.empty())
    {
        messageBox(mfInformation | mfOKButton,
                   "No Lua scripts found. Create one with Lua > New Script... "
                   "(project scripts live in turbo-scripts/).");
        return;
    }
    int n = (int) scripts.size();
    if (n > luaScriptListMax)
        n = luaScriptListMax;
    // Keep the label strings alive until popupMenu returns (TMenuItem may not
    // own them), so build them up front in a vector that outlives the popup.
    std::vector<std::string> labels;
    labels.reserve(n);
    for (int i = 0; i < n; ++i)
        labels.push_back(std::filesystem::path(scripts[i]).filename().string());

    TMenuItem *head = nullptr, *tail = nullptr;
    auto append = [&] (TMenuItem *it) {
        if (!head) head = tail = it;
        else { tail->next = it; tail = it; }
    };
    // Project scripts first, then a separator, then the global scripts. All items
    // are selectable (no disabled headers, which would trap the menu cursor).
    bool sepDone = false;
    for (int i = 0; i < n; ++i)
    {
        bool isProject = !projectRoot.empty() && scripts[i].rfind(projectRoot, 0) == 0;
        if (!isProject && !sepDone && head)
        {
            append(&newLine()); // divide project scripts from global ones
            sepDone = true;
        }
        append(new TMenuItem(labels[i].c_str(), cmLuaScriptBase + i, kbNoKey, hcNoContext));
    }
    TPoint where = { max(0, deskTop->size.x / 2 - 12), 1 };
    unsigned short cmd = popupMenu(where, *head, nullptr);
    int idx = (int) cmd - cmLuaScriptBase;
    if (idx >= 0 && idx < n && luaMgr)
        luaMgr->runFile(scripts[idx]);
}

void TurboApp::runDiscoveredLuaScript(int index) noexcept
{
    if (!luaMgr || index < 0)
        return;
    std::vector<std::string> scripts = discoverLuaScripts();
    if (index < (int) scripts.size())
        luaMgr->runFile(scripts[index]);
}

void TurboApp::treeNewLuaScript(const std::string &dir) noexcept
{
    if (dir.empty())
    {
        messageBox("No scripts directory; cannot create a script.", mfError | mfOKButton);
        return;
    }
    char name[256] = "";
    if (inputBox("New Lua Script", "Script ~n~ame:", name, sizeof(name) - 1) != cmOK)
        return;
    std::string n = trimmed(name);
    if (n.empty())
        return;
    if (n.size() < 4 || n.substr(n.size() - 4) != ".lua")
        n += ".lua";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string full = dir + "/" + n;
    if (std::filesystem::exists(full, ec))
    {
        messageBox(mfError | mfOKButton, "'%s' already exists.", n.c_str());
        return;
    }
    {
        std::ofstream f(full, std::ios::out);
        if (!f)
        {
            messageBox(mfError | mfOKButton, "Unable to create '%s'.", full.c_str());
            return;
        }
        f << "-- " << n << "\n"
             "-- A turboIDE Lua script. Run it from the Lua menu (Run Script...).\n\n"
             "turbo.message(\"Hello from \" .. tostring(turbo.version()))\n";
    }
    fileOpenOrNew(full.c_str());
    refreshLuaScriptsInTree(); // show the new script under its home
}

void TurboApp::treeNewSkill(const std::string &dir) noexcept
{
    if (dir.empty())
    {
        messageBox("No skills directory; cannot create a skill.", mfError | mfOKButton);
        return;
    }
    char name[256] = "";
    if (inputBox("New Skill", "Skill ~n~ame:", name, sizeof(name) - 1) != cmOK)
        return;
    std::string n = trimmed(name);
    if (n.empty())
        return;
    std::error_code ec;
    std::string skillDir = dir + "/" + n;
    if (std::filesystem::exists(skillDir, ec))
    {
        messageBox(mfError | mfOKButton, "'%s' already exists.", n.c_str());
        return;
    }
    std::filesystem::create_directories(skillDir, ec);
    std::string full = skillDir + "/SKILL.md";
    {
        std::ofstream f(full, std::ios::out);
        if (!f)
        {
            messageBox(mfError | mfOKButton, "Unable to create '%s'.", full.c_str());
            return;
        }
        // A Claude-Code-style skill: YAML frontmatter + body. The body points at
        // the turboIDE MCP tools so a skill can drive the editor / run Lua.
        f << "---\n"
             "name: " << n << "\n"
             "description: <one line: what this skill does and when to use it>\n"
             "---\n\n"
             "# " << n << "\n\n"
             "Describe the steps here.\n\n"
             "This agent can drive turboIDE and run project Lua scripts through the\n"
             "turboIDE MCP tools (e.g. `open_file`, `insert_text`, `run_command`, or\n"
             "a `lua_<name>` tool registered via `turbo.register_command`).\n";
    }
    fileOpenOrNew(full.c_str());
    // Add just the new skill folder, rather than rebuilding both homes: a full
    // reinject frees and re-scans every skill, throwing away the expand state and
    // editor links of the ones already open. addNode() scans the new folder (the
    // SKILL.md is already written) and tags it as a skill.
    if (docTree)
        docTree->tree->addNode(skillDir, true);
    // The global skills dir may not have existed at startup (so nothing watched
    // it); if this call just created it, start watching now.
    startGlobalSkillsWatch();
}

void TurboApp::luaNewScript() noexcept
{
    if (projectRoot.empty())
    {
        messageBox("No project directory; cannot create a script.", mfError | mfOKButton);
        return;
    }
    // The Lua menu's "New Script..." defaults to the project home.
    treeNewLuaScript(projectLuaHome(projectRoot));
}

void TurboApp::reloadLuaConfig() noexcept
{
    if (!luaMgr)
    {
        initLua();
        messageBox("Lua initialised.", mfInformation | mfOKButton);
        return;
    }
    int ran = luaMgr->loadInitScripts(projectLuaHome(projectRoot), globalLuaHome());
    messageBox(mfInformation | mfOKButton, "Reloaded %d Lua init script(s).", ran);
}

void TurboApp::refreshLuaScriptsInTree() noexcept
{
    if (!docTree)
        return;
    // The Lua homes are permanent fixtures (always shown), so each is a clear place
    // to keep scripts -- even when empty. Two of them, laid out like the two Skills
    // homes: the project's (committed with the repo), then the user's global one.
    std::vector<DocumentTreeView::LuaSection> sections;
    if (!projectRoot.empty())
    {
        std::string dir = projectLuaHome(projectRoot);
        std::vector<std::string> files = luaHomeFiles(dir, dir);
        sections.push_back({"Project Lua", std::move(dir), std::move(files)});
    }
    std::string globalHome = globalLuaHome();
    if (!globalHome.empty())
    {
        // "New Script..." on this home creates under ~/.turbo/scripts, where its
        // runnable scripts live -- not beside init.lua at the top of ~/.turbo.
        std::string scriptsDir = globalLuaScriptsDir();
        std::vector<std::string> files = luaHomeFiles(globalHome, scriptsDir);
        sections.push_back({"Global Lua", std::move(scriptsDir), std::move(files)});
    }
    docTree->tree->setLuaScripts(true, std::move(sections));
}

void TurboApp::refreshSkillsInTree() noexcept
{
    if (!docTree)
        return;
    // Agent-native skills. For v1 this is the Claude Code convention:
    // <project>/.claude/skills (project) and ~/.claude/skills (global). Kept as
    // separate labelled homes, mirroring how the Lua tiers are shown. (When
    // other agents are supported, resolve these dirs from the configured agent.)
    //
    // Only the directories are named here: the tree scans them like any other
    // folder, so each skill appears as the folder it really is -- SKILL.md,
    // references/ and all -- rather than as a leaf faking the skill's name.
    std::vector<DocumentTreeView::SkillSection> sections;
    if (!projectRoot.empty())
        sections.push_back({"Project Skills", projectRoot + "/.claude/skills"});
    if (const char *h = ::getenv("HOME"))
        sections.push_back({"Global Skills", std::string(h) + "/.claude/skills"});
    docTree->tree->setSkills(true, std::move(sections));
}

void TurboApp::toggleAutoSave()
{
    settings.autoSaveOnFocusLoss ^= true;
    saveSettings(settings);
    refreshMenuChecks();
}

void TurboApp::toggleHiddenFiles()
{
    settings.showHidden ^= true;
    saveSettings(settings);
    if (docTree)
        docTree->tree->setShowHidden(settings.showHidden);
    // The rescan rebuilt every node, dropping git badges; re-fetch so they
    // reappear on the new nodes.
    if (git)
        git->requestStatus();
    refreshMenuChecks();
}

void TurboApp::refreshMenuChecks() noexcept
{
    auto *bar = (TurboMenuBar *) menuBar;
    if (!bar)
        return;
    TMenu *m = bar->rootMenu();

    // Global toggles.
    setMenuItemCheck(m, cmToggleTree, docTree && (docTree->state & sfVisible));
    setMenuItemCheck(m, cmToggleHidden, settings.showHidden);
    setMenuItemCheck(m, cmToggleAutoSave, settings.autoSaveOnFocusLoss);
    // Colour-mode radio group (exactly one checked; any unknown value = auto).
    setMenuItemCheck(m, cmColorModeFull, settings.colorMode == "full");
    setMenuItemCheck(m, cmColorMode16,   settings.colorMode == "16");
    setMenuItemCheck(m, cmColorModeAuto, settings.colorMode != "full" &&
                                         settings.colorMode != "16");
    // Tree-icon-set radio group (exactly one checked; any unknown value = auto).
    setMenuItemCheck(m, cmTreeIconsNerd,    settings.treeIcons == "nerd");
    setMenuItemCheck(m, cmTreeIconsUnicode, settings.treeIcons == "unicode");
    setMenuItemCheck(m, cmTreeIconsAscii,   settings.treeIcons == "ascii");
    setMenuItemCheck(m, cmTreeIconsAuto,    settings.treeIcons != "nerd" &&
                                            settings.treeIcons != "unicode" &&
                                            settings.treeIcons != "ascii");

    // Per-editor toggles reflect the active (most-recently-focused) editor.
    // With no editor open they all show unchecked.
    EditorWindow *w = MRUlist.empty() ? nullptr : MRUlist.next->self;
    bool lineNums = false, wrap = false, indent = false, chHist = false, edge = false;
    if (w)
    {
        auto &ed = w->getEditor();
        lineNums = ed.lineNumbers.isEnabled();
        wrap     = ed.wrapping.isEnabled();
        indent   = ed.autoIndent.isEnabled();
        chHist   = ed.changeHistoryEnabled;
        edge     = ed.edgeEnabled;
    }
    setMenuItemCheck(m, cmToggleLineNums,      lineNums);
    setMenuItemCheck(m, cmToggleWrap,          wrap);
    setMenuItemCheck(m, cmToggleIndent,        indent);
    setMenuItemCheck(m, cmToggleChangeHistory, chHist);
    setMenuItemCheck(m, cmToggleFolding,       w && w->getEditor().foldingEnabled);
    setMenuItemCheck(m, cmToggleEdge,          edge);
}

void TurboApp::rebuildMenuBar(int recentCount, int toolCount)
{
    if (menuBar)
    {
        remove(menuBar);
        TObject::destroy(menuBar);
    }
    TRect r = getExtent();
    menuBar = makeMenuBar(r, recentCount, toolCount);
    insert(menuBar);
    // The branch indicator sits on top of the full-width menu bar's right end;
    // re-insert it so the freshly inserted menu bar doesn't cover it.
    if (branchView)
    {
        remove(branchView);
        insert(branchView);
    }
    menuRecentCount = recentCount;
    menuToolCount = toolCount;
    fillToolMenuLabels(); // the new bar's tool items start blank
    refreshMenuChecks();  // ... and without check marks
}

void TurboApp::refreshWindowList() noexcept
{
    // Size the recent-window and tool-toggle sections to their current counts,
    // rebuilding the menu bar once when either changes so there are never empty
    // rows (and the two dynamic sections agree with the bar that is built).
    //
    // The agent window is not an editor -- it never joins MRUlist and carries no
    // Alt-1..9 number -- but it is still a window the user switches between, so
    // while it is open it takes the slot after the editors.
    bool agentOpen = (agentWin != nullptr);
    int editors = 0;
    MRUlist.forEach([&] (EditorWindow *w) { if (w) ++editors; });
    int editorMax = windowListMax - (agentOpen ? 1 : 0);
    if (editors > editorMax)
        editors = editorMax;
    int count = editors + (agentOpen ? 1 : 0);
    int toolCount = (int) tools.size();
    if (toolCount > toolListMax)
        toolCount = toolListMax;
    if (count != menuRecentCount || toolCount != menuToolCount)
        rebuildMenuBar(count, toolCount);

    auto *bar = (TurboMenuBar *) menuBar;
    if (!bar)
        return;
    TMenu *m = bar->rootMenu();
    // '~' is TVision's hotkey marker, so a title carrying one has to escape it.
    // (EditorWindow::title is a std::string; TWindow::getTitle returns a char *.)
    auto escapeTildes = [] (std::string &label, TStringView title) {
        for (char c : title)
        {
            if (c == '~') label += '~';
            label += c;
        }
    };
    int i = 0;
    MRUlist.forEach([&] (EditorWindow *w) {
        if (i >= editors || !w)
            return;
        // Entries are MRU-ordered, but each is prefixed with the window's stable
        // 1..9 number -- the same one Alt-1..9 selects -- so the menu and the
        // keyboard shortcut always agree. Windows past nine show no number.
        std::string label = (w->number >= 1 && w->number <= 9)
            ? std::to_string(w->number) + " " : "  ";
        escapeTildes(label, w->title);
        setMenuItemLabel(m, cmWindowBase + i, label.c_str(), true);
        ++i;
    });

    // The agent window's slot, last. It has no Alt-1..9 number, so it is indented
    // to line up with the numbered editors above it, like an editor past nine.
    menuAgentIndex = agentOpen ? i : -1;
    if (agentOpen)
    {
        std::string label = "  ";
        escapeTildes(label, agentWin->getTitle(0));
        setMenuItemLabel(m, cmWindowBase + i, label.c_str(), true);
    }

    // Tick the tool toggles whose process is currently running. Labels are set
    // once (rebuild / config change); here we only maintain the check marks.
    for (int t = 0; t < menuToolCount && t < (int) tools.size(); ++t)
        setMenuItemCheck(m, cmToolBase + t, toolRunning(t));
}

void TurboApp::focusRecentWindow(int index) noexcept
{
    // The agent window's slot sits after the editors (refreshWindowList).
    if (agentWin && index == menuAgentIndex)
    {
        agentWin->focus();
        return;
    }
    int i = 0;
    EditorWindow *target = nullptr;
    MRUlist.forEach([&] (EditorWindow *w) {
        if (i == index)
            target = w;
        ++i;
    });
    if (target)
        target->focus();
}

void TurboApp::configureLsp()
{
    if (!lsp)
        return;
    std::vector<std::pair<std::string, std::string>> servers;
    servers.reserve(settings.lspServers.size());
    for (auto &s : settings.lspServers)
        servers.emplace_back(s.language, s.command);
    lsp->configure(settings.lspEnabled, std::move(servers));
}

void TurboApp::editLspSettings()
{
    if (executeLspDialog(settings))
    {
        saveSettings(settings);
        configureLsp();
    }
}

void TurboApp::editDebugSettings()
{
    if (projectRoot.empty())
    {
        messageBox("Open a project to configure its debuggers (.turbo/debug.json).",
                   mfInformation | mfOKButton);
        return;
    }
    if (executeDebugDialog(debugConfig))
    {
        debugConfig.save(projectRoot);
        if (dap)
            dap->setConfig(debugConfig);
    }
}

void TurboApp::editThemeSettings()
{
    // The dialog mutates the active schemes and posts cmApplyTheme itself on
    // Apply/OK (handled synchronously by applyActiveTheme), so there's nothing
    // to do here on return. Cancel leaves the active schemes untouched.
    executeThemeDialog();
}

void TurboApp::applyActiveTheme() noexcept
{
    // Persist only the diffs from the built-in defaults.
    storeThemeToSettings(settings);
    saveSettings(settings);
    // Re-theme every open editor from the (now updated) active scheme, routed
    // through the per-window active-state theming so windows with their own
    // surface (Lua brown, spec purple) keep it rather than snapping back to
    // the plain scheme until the next focus change.
    MRUlist.forEach([] (EditorWindow *w) {
        if (w)
            w->applyActiveStateTheme();
    });
    // Repaint the window chrome (frames/scrollbars read windowSchemeActive).
    if (deskTop)
        deskTop->redraw();
}

void TurboApp::setColorMode(const char *mode) noexcept
{
    if (settings.colorMode == mode)
        return;
    // Whether Turbo Vision's colour-depth cap (TVISION_COLORS=16, applied at
    // start-up) matches what the new mode wants. If it doesn't, views that draw
    // with raw RGB attributes (the file tree, debug panels) can't fully switch
    // until the next launch.
    const char *capEnv = ::getenv("TVISION_COLORS");
    bool capNow = capEnv && std::string(capEnv) == "16";

    settings.colorMode = mode;
    saveSettings(settings);

    // Re-select the active schemes (classic BIOS vs 24-bit RGB) and repaint.
    // The editor, window chrome, menus and dialogs switch immediately: BIOS
    // colours render through the terminal's own 16-colour palette regardless of
    // the detected depth, and getPalette() re-reads the mode on each redraw.
    applyThemeFromSettings(settings);
    // Route through the per-window theming so Lua-brown and spec-purple
    // surfaces re-derive their backgrounds for the new mode (e.g. the spec
    // surface swaps between RGB violet and BIOS magenta).
    MRUlist.forEach([] (EditorWindow *w) {
        if (w)
            w->applyActiveStateTheme();
    });
    if (deskTop)
        deskTop->redraw();
    refreshMenuChecks();

    if (capNow != (settings.colorMode == "16"))
        messageBox(mfInformation | mfOKButton,
                   "Colour mode set to \"%s\". Restart Turbo to apply it fully "
                   "(the file tree and debug panels update on the next launch).",
                   mode);
}

void TurboApp::setTreeIcons(const char *mode) noexcept
{
    if (settings.treeIcons == mode)
        return;
    settings.treeIcons = mode;
    saveSettings(settings);
    // Re-resolve the active glyph set and repaint the tree. Fully live: the set
    // is read at draw time and the icon column can widen/collapse, so the tree
    // is relaid out (update) as well as redrawn. "nerd" needs a Nerd Font in the
    // terminal; without one its pictograms show as blank boxes.
    setTreeIconSet(parseTreeIconSet(settings.treeIcons));
    if (docTree)
        docTree->tree->refreshIcons();
    refreshMenuChecks();
}

void TurboApp::gitRefresh()
{
    // The explicit "Refresh Status" menu command echoes a human-readable
    // `git status` to the output pane (and refreshes the tree badges). The
    // automatic refreshes elsewhere call requestStatus() directly and stay silent.
    if (!git || !git->isRepo())
    {
        messageBox("This folder is not a git repository.", mfInformation | mfOKButton);
        return;
    }
    git->statusToOutput();
}

void TurboApp::startGlobalSkillsWatch() noexcept
{
    if (!skillsWatcher || globalSkillsDir.empty())
        return;
    std::error_code ec;
    if (std::filesystem::is_directory(globalSkillsDir, ec))
        skillsWatcher->start(globalSkillsDir);
}

void TurboApp::onFilesChanged()
{
    std::vector<std::string> changed, globalSkills;
    bool any = false;
    if (watcher && watcher->poll(changed))
        any = true;
    if (skillsWatcher && skillsWatcher->poll(globalSkills))
        any = true;
    if (!any)
        return;

    // Reconcile one changed path with the tree and any editor holding it.
    auto reconcile = [this] (const std::string &p) {
        // If this file is open in an editor, reconcile the buffer with the new
        // on-disk contents (silent reload when clean, prompt when dirty).
        handleExternalFileChange(p);
        // Skip git's own internals for tree structure (they are hidden anyway);
        // they only matter for the status refresh below. Check both separators
        // so the Windows watcher's backslash paths match too.
        if (p.find("/.git/") != std::string::npos ||
            p.find("\\.git\\") != std::string::npos)
            return;
        if (!docTree)
            return;
        std::error_code ec;
        if (std::filesystem::exists(p, ec))
        {
            bool isDir = std::filesystem::is_directory(p, ec);
            // No-op if already present or hidden; adds a node for new files/dirs.
            docTree->tree->addNode(p, isDir);
        }
        else
            docTree->tree->removeNode(p); // no-op if not in the tree
    };

    // Any change under the worktree may affect git status. Global skills live
    // outside the repo, so they never do.
    bool gitTouched = !changed.empty();
    for (auto &p : changed)
        reconcile(p);
    for (auto &p : globalSkills)
        reconcile(p);
    if (gitTouched && git)
        git->requestStatus();
}

static std::string trimmed(const char *s)
{
    std::string v = s;
    auto a = v.find_first_not_of(" \t");
    if (a == std::string::npos)
        return {};
    auto b = v.find_last_not_of(" \t");
    return v.substr(a, b - a + 1);
}

void TurboApp::treeCreateFile(const std::string &dirPath)
{
    char name[256] = "";
    if (inputBox("New File", "File ~n~ame:", name, sizeof(name) - 1) != cmOK)
        return;
    std::string n = trimmed(name);
    if (n.empty())
        return;
    std::string full = dirPath + "/" + n;
    std::error_code ec;
    if (std::filesystem::exists(full, ec))
    {
        messageBox(mfError | mfOKButton, "'%s' already exists.", n.c_str());
        return;
    }
    // Allow a relative path in the name (e.g. "sub/new.txt"): make the parents.
    auto parent = std::filesystem::path(full).parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, ec);
    {
        std::ofstream f(full, std::ios::out);
        if (!f)
        {
            messageBox(mfError | mfOKButton, "Unable to create '%s'.", full.c_str());
            return;
        }
    }
    // Show the new node immediately (the watcher would also pick it up), then
    // open it for editing.
    if (docTree)
        docTree->tree->addNode(full, false);
    fileOpenOrNew(full.c_str());
    if (git)
        git->requestStatus();
}

void TurboApp::treeCreateFolder(const std::string &dirPath)
{
    char name[256] = "";
    if (inputBox("New Folder", "Folder ~n~ame:", name, sizeof(name) - 1) != cmOK)
        return;
    std::string n = trimmed(name);
    if (n.empty())
        return;
    std::string full = dirPath + "/" + n;
    std::error_code ec;
    if (std::filesystem::exists(full, ec))
    {
        messageBox(mfError | mfOKButton, "'%s' already exists.", n.c_str());
        return;
    }
    // Allow nested paths in the name (e.g. "a/b/c").
    std::filesystem::create_directories(full, ec);
    if (ec)
    {
        messageBox(mfError | mfOKButton, "Unable to create '%s'.", full.c_str());
        return;
    }
    // Show the new directory immediately (the watcher would also pick it up).
    // Git tracks files, not empty directories, so there is no status to refresh.
    if (docTree)
        docTree->tree->addNode(full, true);
}

void TurboApp::treeRenamePath(const std::string &path, bool isDir)
{
    TStringView baseSv = TPath::basename(path);
    std::string base {baseSv.data(), baseSv.size()};
    char name[256] = "";
    strnzcpy(name, base.c_str(), sizeof name);
    if (inputBox(isDir ? "Rename Folder" : "Rename File",
                 "New ~n~ame:", name, sizeof(name) - 1) != cmOK)
        return;
    std::string n = trimmed(name);
    if (n.empty() || n == base)
        return;
    TStringView dirSv = TPath::dirname(path);
    std::string newPath {dirSv.data(), dirSv.size()};
    newPath += "/";
    newPath += n;
    std::error_code ec;
    if (std::filesystem::exists(newPath, ec))
    {
        messageBox(mfError | mfOKButton, "'%s' already exists.", n.c_str());
        return;
    }
    std::filesystem::rename(path, newPath, ec);
    if (ec)
    {
        messageBox(mfError | mfOKButton, "Unable to rename: %s.", ec.message().c_str());
        return;
    }
    // Re-point any open editor whose file is the renamed file, or lives inside
    // the renamed directory, at its new location.
    std::vector<EditorWindow *> affected;
    std::string oldPrefix = path + "/";
    std::string newPrefix = newPath + "/";
    MRUlist.forEach([&] (EditorWindow *w) {
        if (!w)
            return;
        const std::string &fp = w->filePath();
        std::string np;
        if (!isDir && fp == path)
            np = newPath;
        else if (isDir && fp.size() > oldPrefix.size() &&
                 fp.compare(0, oldPrefix.size(), oldPrefix) == 0)
            np = newPrefix + fp.substr(oldPrefix.size());
        else
            return;
        w->getEditor().filePath = np;
        w->getEditor().onFilePathSet(); // re-detect language for the new name
        affected.push_back(w);
    });
    // Rebuild the affected part of the tree (the watcher would reconcile this
    // eventually, but do it now so the rename is immediate and links are kept).
    if (docTree)
    {
        docTree->tree->removeNode(path);
        docTree->tree->addNode(newPath, isDir);
        for (auto *w : affected)
        {
            docTree->tree->linkEditor(w);   // attach to the freshly created node
            handleTitleChange(*w);          // renumber/retitle a renamed file
        }
    }
    if (git)
        git->requestStatus();
}

void TurboApp::treeStagePath(const std::string &path)
{
    if (!git || !git->isRepo())
    {
        messageBox("This folder is not a git repository.", mfInformation | mfOKButton);
        return;
    }
    git->stage({path}, [] (int code, const std::string &output) {
        if (code != 0)
        {
            std::string m = "git add failed:\n" +
                (output.empty() ? std::string("(see terminal)") : output.substr(0, 400));
            messageBox(m.c_str(), mfError | mfOKButton);
        }
    });
    // GitManager queues a status refresh after staging, so the badge updates.
}

void TurboApp::treeRevertPath(const std::string &path)
{
    if (!git || !git->isRepo())
    {
        messageBox("This folder is not a git repository.", mfInformation | mfOKButton);
        return;
    }
    TStringView baseSv = TPath::basename(path);
    std::string base {baseSv.data(), baseSv.size()};
    // Discarding local changes is irreversible, so confirm first.
    if (messageBox(mfConfirmation | mfYesButton | mfNoButton,
                   "Discard all local changes to '%s'?\nThis cannot be undone.",
                   base.c_str()) != cmYes)
        return;
    std::string p = path;
    ++suppressExternalReload; // this reload is ours; keep the watcher out of it
    git->revert({p}, [this, p] (int code, const std::string &output) {
        --suppressExternalReload;
        if (code != 0)
        {
            std::string m = "git revert failed:\n" +
                (output.empty() ? std::string("(see terminal)") : output.substr(0, 400));
            messageBox(m.c_str(), mfError | mfOKButton);
            return;
        }
        // The file on disk is now the committed version; pull it into any open
        // editor so the buffer matches (and a later save doesn't undo the revert).
        reloadEditorFromDisk(p);
    });
    // GitManager queues a status refresh after the checkout, clearing the badge.
}

// Read 'path's current modification time and size. Returns false -- leaving the
// outputs untouched -- if it can't be stat'd (e.g. the file was removed).
static bool statFileSig( const std::string &path,
                         std::filesystem::file_time_type &mtime,
                         std::uintmax_t &size ) noexcept
{
    std::error_code ec;
    auto t = std::filesystem::last_write_time(path, ec);
    if (ec)
        return false;
    auto s = std::filesystem::file_size(path, ec);
    if (ec)
        return false;
    mtime = t;
    size = s;
    return true;
}

void TurboApp::rememberDiskSignature(EditorWindow &w) noexcept
{
    if (w.filePath().empty())
    {
        w.diskSigValid = false;
        return;
    }
    w.diskSigValid = statFileSig(w.filePath(), w.diskModTime, w.diskSize);
}

void TurboApp::handleExternalFileChange(const std::string &path) noexcept
{
    // A git operation that rewrites the tree reloads its own editors from its
    // callback; don't second-guess it (avoids a double reload / redundant prompt).
    if (suppressExternalReload > 0)
        return;
    EditorWindow *target = nullptr;
    MRUlist.forEach([&] (EditorWindow *w) {
        if (w && w->filePath() == path)
            target = w;
    });
    if (!target)
        return;
    std::filesystem::file_time_type mtime;
    std::uintmax_t size;
    if (!statFileSig(path, mtime, size))
        return; // removed or unreadable: keep the buffer as-is
    if (target->diskSigValid &&
        mtime == target->diskModTime && size == target->diskSize)
        return; // unchanged since we last read/wrote it (e.g. our own save)
    auto &ed = target->getEditor();
    if (ed.inSavePoint())
    {
        // No unsaved edits: silently pull in the new contents.
        reloadEditorFromDisk(path); // also refreshes the stored signature
        return;
    }
    // Unsaved edits would be lost by a reload, so ask first.
    ushort reply = messageBox( mfWarning | mfYesButton | mfNoButton,
        "'%s' has been changed on disk by another program.\n"
        "Reload it and discard your unsaved changes?", path.c_str() );
    if (reply == cmYes)
        reloadEditorFromDisk(path);
    else
        // Keep the user's version, but adopt the current file as the new
        // baseline so we don't ask again for this same change.
        rememberDiskSignature(*target);
}

void TurboApp::reloadEditorFromDisk(const std::string &path) noexcept
{
    EditorWindow *target = nullptr;
    MRUlist.forEach([&] (EditorWindow *w) {
        if (w && w->filePath() == path)
            target = w;
    });
    if (!target)
        return;
    auto &ed = target->getEditor();
    ed.callScintilla(SCI_CLEARALL, 0U, 0U);
    turbo::readFile(ed.scintilla, path.c_str(), turbo::showNoDialogs);
    ed.callScintilla(SCI_EMPTYUNDOBUFFER, 0U, 0U);
    ed.callScintilla(SCI_SETSAVEPOINT, 0U, 0U); // mark the buffer clean (= on disk)
    ed.redraw();
    rememberDiskSignature(*target); // buffer now matches disk; re-baseline
}

void TurboApp::reloadCleanEditorsFromDisk() noexcept
{
    MRUlist.forEach([&] (EditorWindow *w) {
        if (!w || w->filePath().empty())
            return;
        auto &ed = w->getEditor();
        if (!ed.inSavePoint())
            return; // unsaved buffer: leave the user's work alone
        std::error_code ec;
        if (!std::filesystem::exists(w->filePath(), ec))
            return; // file absent on the new branch: keep what's in the editor
        ed.callScintilla(SCI_CLEARALL, 0U, 0U);
        turbo::readFile(ed.scintilla, w->filePath().c_str(), turbo::showNoDialogs);
        ed.callScintilla(SCI_EMPTYUNDOBUFFER, 0U, 0U);
        ed.callScintilla(SCI_SETSAVEPOINT, 0U, 0U);
        ed.redraw();
        rememberDiskSignature(*w); // buffer now matches disk; re-baseline
    });
}

void TurboApp::refreshBranchView() noexcept
{
    if (!branchView)
        return;
    // Text: branch icon (U+2387) + current branch (or a placeholder).
    std::string text = "\xE2\x8E\x87 ";
    bool repo = git && git->isRepo();
    if (!repo)
        text += "no repo";
    else
    {
        const std::string &cur = git->currentStatus().branch;
        text += cur.empty() ? "\xE2\x80\xA6" : cur; // ellipsis: not loaded yet
    }
    if (text == branchTextShown)
        return;
    branchTextShown = text;
    branchView->setText(text.c_str());
    // Size the indicator to its text and keep it pinned to the right edge.
    int w = strwidth(text.c_str());
    TRect ext = getExtent();
    TRect r {ext.b.x - w, ext.a.y, ext.b.x, ext.a.y + 1};
    branchView->locate(r);
}

void TurboApp::showBranchMenu(TPoint where) noexcept
{
    if (!git || !git->isRepo())
    {
        messageBox("This folder is not a git repository.", mfInformation | mfOKButton);
        return;
    }
    // Build the list of the OTHER branches fresh each time (no persistent items).
    const GitRepoStatus &st = git->currentStatus();
    std::vector<std::string> others;
    for (auto &b : st.branches)
        if (b != st.branch)
            others.push_back(b);

    TMenuItem *head = nullptr, *tail = nullptr;
    auto append = [&] (TMenuItem *it) {
        if (!head) head = tail = it;
        else { tail->next = it; tail = it; }
    };
    int n = (int) others.size();
    if (n > branchListMax)
        n = branchListMax;
    for (int i = 0; i < n; ++i)
        append(new TMenuItem(others[i].c_str(), cmBranchBase + i, kbNoKey, hcNoContext));
    if (!head)
    {
        auto *it = new TMenuItem("(no other branches)", cmBranchBase, kbNoKey, hcNoContext);
        it->disabled = True;
        append(it);
    }
    append(&newLine());
    append(new TMenuItem("New branch...", cmGitNewBranch, kbNoKey, hcNoContext));

    // popupMenu takes ownership of the chain (wraps it in a TMenu it deletes).
    unsigned short cmd = popupMenu(where, *head, nullptr);
    if (cmd == cmGitNewBranch)
        gitNewBranch();
    else
    {
        int idx = (int) cmd - cmBranchBase;
        if (idx >= 0 && idx < n)
            switchToBranch(others[idx]);
    }
}

void TurboApp::switchToBranch(const std::string &branch) noexcept
{
    if (!git || !git->isRepo())
        return;

    // "Dirty" for checkout purposes = any change to a tracked file. Untracked
    // files don't block a normal checkout, so they don't trigger the prompt.
    bool dirty = false;
    for (auto &kv : git->currentStatus().files)
        if (kv.second.state != GitFileState::Untracked)
        {
            dirty = true;
            break;
        }

    auto onDone = [this, branch] (int code, const std::string &output) {
        --suppressExternalReload;
        if (code != 0)
        {
            std::string m = "Could not switch to '" + branch + "':\n" +
                (output.empty() ? std::string("(see terminal)") : output.substr(0, 400));
            messageBox(m.c_str(), mfError | mfOKButton);
            return;
        }
        // Working tree now matches the new branch; refresh editors to suit.
        reloadCleanEditorsFromDisk();
    };
    // The checkout rewrites the tree and onDone reloads the editors; keep the
    // watcher-driven handler out of it for the duration.
    auto startSwitch = [&] (GitManager::SwitchMode mode) {
        ++suppressExternalReload;
        git->switchBranch(branch, mode, onDone);
    };

    if (!dirty)
    {
        startSwitch(GitManager::SwitchMode::Plain);
        return;
    }
    unsigned short choice = executeBranchSwitchDialog(branch.c_str());
    if (choice == cmYes)
        startSwitch(GitManager::SwitchMode::Stash);
    else if (choice == cmNo)
        startSwitch(GitManager::SwitchMode::Force);
    // cmCancel (or closing the dialog): stay on the current branch.
}

void TurboApp::gitNewBranch()
{
    if (!git || !git->isRepo())
    {
        messageBox("This folder is not a git repository.", mfInformation | mfOKButton);
        return;
    }
    std::string name;
    if (!executeNewBranchDialog(name))
        return;
    // `checkout -b` keeps the working tree, so open editors need no reloading;
    // the queued status refresh updates the branch indicator and dropdown.
    git->createBranch(name, [name] (int code, const std::string &output) {
        if (code != 0)
        {
            std::string m = "Could not create branch '" + name + "':\n" +
                (output.empty() ? std::string("(see Git output)") : output.substr(0, 400));
            messageBox(m.c_str(), mfError | mfOKButton);
        }
    });
}

void TurboApp::gitCommitDialog()
{
    if (!git || !git->isRepo())
    {
        messageBox("This folder is not a git repository.", mfInformation | mfOKButton);
        return;
    }
    executeGitCommitDialog(*git,
        [this] (const std::string &message) {
            // A Lua beforeCommit handler may veto by returning false.
            return fireLuaEvent("beforeCommit", {{"message", message}});
        },
        [this] (bool ok, const std::string &output) {
            fireLuaEvent("afterCommit", {{"ok", ok ? "true" : "false"},
                                         {"output", output}});
        });
    // GitManager queues a status refresh after committing.
}

void TurboApp::gitRemote(int which)
{
    if (!git || !git->isRepo())
    {
        messageBox("This folder is not a git repository.", mfInformation | mfOKButton);
        return;
    }
    auto report = [] (const char *what) {
        return [what] (int code, const std::string &output) {
            if (code != 0)
            {
                std::string msg = std::string("git ") + what + " failed:\n" +
                    (output.empty() ? "(see terminal)" : output.substr(0, 400));
                messageBox(msg.c_str(), mfError | mfOKButton);
            }
        };
    };
    switch (which)
    {
        case 0: git->fetch(report("fetch")); break;
        case 1: git->pull(report("pull")); break;
        case 2: git->push(report("push")); break;
    }
}

void TurboApp::gitMerge()
{
    if (!git || !git->isRepo())
    {
        messageBox("This folder is not a git repository.", mfInformation | mfOKButton);
        return;
    }
    if (git->currentStatus().merging)
    {
        messageBox("A merge is already in progress. Resolve the conflicts and "
                   "Continue Merge, or Abort Merge first.", mfInformation | mfOKButton);
        return;
    }
    std::string branch;
    int favor = 0;
    if (!executeMergeDialog(*git, branch, favor))
        return;
    GitManager::MergeFavor f = favor == 1 ? GitManager::MergeFavor::Ours
                             : favor == 2 ? GitManager::MergeFavor::Theirs
                                          : GitManager::MergeFavor::None;
    ++suppressExternalReload; // our own reload; keep the watcher handler out of it
    git->merge(branch, f, [this, branch] (int code, const std::string &output) {
        --suppressExternalReload;
        // The working tree changed either way (a merge commit, or conflict
        // markers written into the files), so reload clean editors from disk.
        reloadCleanEditorsFromDisk();
        bool conflicted = output.find("CONFLICT") != std::string::npos ||
                          output.find("Automatic merge failed") != std::string::npos;
        if (conflicted)
            messageBox("Merge has conflicts. Resolve the files marked 'U' (open "
                       "one and use the editor's conflict toolbar to take ours / "
                       "theirs per change), then Git > Continue Merge -- or "
                       "Git > Abort Merge to back out.", mfInformation | mfOKButton);
        else if (code != 0)
        {
            std::string m = "git merge '" + branch + "' failed:\n" +
                (output.empty() ? std::string("(see Git output)") : output.substr(0, 400));
            messageBox(m.c_str(), mfError | mfOKButton);
        }
    });
}

void TurboApp::gitMergeAbort()
{
    if (!git || !git->isRepo())
    {
        messageBox("This folder is not a git repository.", mfInformation | mfOKButton);
        return;
    }
    ++suppressExternalReload; // our own reload; keep the watcher handler out of it
    git->mergeAbort([this] (int code, const std::string &output) {
        --suppressExternalReload;
        reloadCleanEditorsFromDisk();
        if (code != 0)
        {
            std::string m = "git merge --abort failed:\n" +
                (output.empty() ? std::string("(no merge in progress?)") : output.substr(0, 400));
            messageBox(m.c_str(), mfError | mfOKButton);
        }
    });
}

void TurboApp::gitMergeContinue()
{
    if (!git || !git->isRepo())
    {
        messageBox("This folder is not a git repository.", mfInformation | mfOKButton);
        return;
    }
    ++suppressExternalReload; // our own reload; keep the watcher handler out of it
    git->mergeContinue([this] (int code, const std::string &output) {
        --suppressExternalReload;
        reloadCleanEditorsFromDisk();
        if (code != 0)
        {
            std::string m = "Could not complete the merge:\n" +
                (output.empty() ? std::string("(resolve all 'U' files first)")
                                : output.substr(0, 400));
            messageBox(m.c_str(), mfError | mfOKButton);
        }
    });
}

void TurboApp::gitResolveFile(EditorWindow *w) noexcept
{
    if (!w || !git || !git->isRepo())
        return;
    // Save the resolved file to disk (synchronous), then stage it so git marks
    // the path resolved; the next status refresh clears the conflict toolbar.
    message(w, evCommand, cmSave, nullptr);
    std::string path = w->filePath();
    git->stage({path}, [] (int code, const std::string &output) {
        if (code != 0)
        {
            std::string m = "git add failed:\n" +
                (output.empty() ? std::string("(see Git output)") : output.substr(0, 400));
            messageBox(m.c_str(), mfError | mfOKButton);
        }
    });
}

void TurboApp::updateEditorConflictBars() noexcept
{
    if (!git)
        return;
    const auto &files = git->currentStatus().files;
    MRUlist.forEach([&] (EditorWindow *w) {
        if (!w)
            return;
        auto it = files.find(w->filePath());
        bool conflicted = it != files.end() &&
                          it->second.state == GitFileState::Conflicted;
        w->setConflictMode(conflicted);
    });
}

void TurboApp::closeAll()
{
    while (!MRUlist.empty()) {
        auto *head = MRUlist.next;
        message((EditorWindow *) head->self, evCommand, cmClose, 0);
        TScreen::flushScreen();
        if (MRUlist.next == head) // Not removed
            break;
    }
}

TRect TurboApp::newEditorBounds() const
{
    if (!MRUlist.empty())
        return MRUlist.next->self->getBounds();
    else {
        TRect r = deskTop->getExtent();
        if (docTree && docTree->state & sfVisible) {
            TRect t = docTree->getBounds();
            // Align left.
            if (t.a.x > r.b.x - t.b.x)
                r.b.x = max(t.a.x, EditorWindow::minSize.x);
            // Align right.
            else
                r.a.x = min(t.b.x, r.b.x - EditorWindow::minSize.x);
        }
        // Leave room for the docked output pane at the bottom.
        if (outputWin && (outputWin->state & sfVisible))
            r.b.y = min(r.b.y, outputBounds().a.y);
        return r;
    }
}

turbo::TScintilla &TurboApp::createScintilla() noexcept
{
    return turbo::createScintilla();
}

void TurboApp::addEditor(turbo::TScintilla &scintilla, const char *path)
// Pre: 'path' is an absolute path.
{
    TRect r = newEditorBounds();
    auto &counter = fileCount[TPath::basename(path)];
    auto &editor = *new TurboEditor(scintilla, path);
    EditorWindow &w = *new EditorWindow(r, editor, counter, searchSettings, *this);
    // Files under <project-root>/specs/ get the purple spec surface. Decided
    // once here; applyActiveStateTheme() reads the flag on every focus change.
    {
        std::string rel = relInProject(projectRoot, path ? path : "");
        w.isSpec = rel == "specs" || rel.rfind("specs/", 0) == 0;
    }
    // Give it a stable 1..9 number for Turbo Vision's built-in Alt-1..9 window
    // selection. Assigned before inserting into the MRU list so the new window
    // isn't counted as already using a number; it stays fixed for the window's
    // lifetime and is freed for reuse when the window closes.
    w.number = lowestFreeWindowNumber();
    if (docTree)
        docTree->tree->linkEditor(&w);
    w.listHead.insert_after(&MRUlist);
    // Baseline for external-change detection: the file as it is on disk right now.
    rememberDiskSignature(w);
    deskTop->insert(&w);
    enableCommands(editorCmds);
    if (lsp)
        lsp->didOpen(w);
    // Lua hooks: an unnamed scratch buffer (empty path) is a "newFile"; anything
    // file-backed (including a freshly-created named file) is an "openFile".
    if (path && path[0])
        fireLuaEvent("openFile", {{"path", path}});
    else
        fireLuaEvent("newFile");
}

short TurboApp::lowestFreeWindowNumber() noexcept
{
    bool used[10] = {}; // index 1..9
    MRUlist.forEach([&] (EditorWindow *win) {
        if (win && win->number >= 1 && win->number <= 9)
            used[win->number] = true;
    });
    for (short n = 1; n <= 9; ++n)
        if (!used[n])
            return n;
    return wnNoNumber; // more than 9 windows: extras are unnumbered
}

void TurboApp::showEditorList(TEvent *ev)
{
    EditorListModel model {MRUlist};
    TRect r {0, 0, 0, 0};
    r.b.x = min(max(ListModel::maxItemCStrLen(model) + 4, 40), deskTop->size.x - 10);
    r.b.y = min(max(model.size() + 2, 6), deskTop->size.y - 4);
    r.move((deskTop->size.x - r.b.x) / 2,
           (deskTop->size.y - r.b.y) / 4);
    ListWindow *lw = &ListWindow::create<EditorListView>(r, "Buffer List", model, lvScrollBars);
    if (ev)
        lw->putEvent(*ev);
    if (deskTop->execView(lw) == cmOK)
        if (auto *wnd = (EditorWindow *) lw->getCurrent())
            wnd->focus();

    destroy(lw);
}

void TurboApp::toggleTreeView()
{
    if (!docTree)
        return;
    // Prevent editors from doing draw() on each changeBounds(). We'll draw all
    // the views at the end.
    MRUlist.forEach([&] (auto *win) {
        win->setState(sfExposed, False);
    });
    TRect dr = docTree->getBounds();
    if (docTree->state & sfVisible) {
        docTree->hide();
        MRUlist.forEach([this, dr] (auto *win) {
            TRect r = win->getBounds();
            if (r.a.x >= dr.b.x)
                r.a.x -= docTree->size.x;
            else if (r.b.x <= dr.a.x)
                r.b.x += docTree->size.x;
            win->locate(r);
        });
    } else {
        MRUlist.forEach([this, dr] (auto *win) {
            TRect r = win->getBounds();
            if (r.a.x + docTree->size.x >= dr.b.x)
                r.a.x += docTree->size.x;
            else if (r.b.x - docTree->size.x <= dr.a.x)
                r.b.x -= docTree->size.x;
            win->locate(r);
        });
        docTree->show();
    }
    // The output pane spans the editor area, so its width follows the tree.
    if (outputWin && (outputWin->state & sfVisible))
    {
        TRect ob = outputBounds();
        outputWin->locate(ob);
    }
    MRUlist.forEach([&] (auto *win) {
        win->setState(sfExposed, True);
    });
    deskTop->redraw();
}

void TurboApp::setTreeWidth(int w)
{
    if (!docTree || !(docTree->state & sfVisible))
        return;
    TRect ext = deskTop->getExtent();
    int totalW = ext.b.x - ext.a.x;
    // Keep the tree at least a window's minimum width (minWinSize.x = 16, so
    // locate() never clamps it wider than asked) and leave an editor's minimum
    // width beside it.
    w = max(16, min(w, totalW - EditorWindow::minSize.x));
    int oldLeft = docTree->getBounds().a.x;
    int newLeft = ext.b.x - w;
    if (newLeft == oldLeft)
        return;
    // Suppress per-editor redraws while we relocate; draw once at the end.
    MRUlist.forEach([&] (auto *win) { win->setState(sfExposed, False); });
    // Editors whose right edge sat on the tree's left edge follow it (narrowing
    // the tree grows them, widening shrinks them).
    MRUlist.forEach([&] (auto *win) {
        TRect r = win->getBounds();
        if (r.b.x >= oldLeft)
        {
            r.b.x = newLeft;
            win->locate(r);
        }
    });
    TRect tb = docTree->getBounds();
    tb.a.x = newLeft;
    docTree->locate(tb);
    // The output pane spans the editor area, so its width follows the tree too.
    if (outputWin && (outputWin->state & sfVisible))
    {
        TRect ob = outputBounds();
        outputWin->locate(ob);
    }
    MRUlist.forEach([&] (auto *win) { win->setState(sfExposed, True); });
    deskTop->redraw();
}

TRect TurboApp::outputBounds() const
{
    TRect ext = deskTop->getExtent();
    int totalH = ext.b.y - ext.a.y;
    // User-set height once dragged, else ~1/5 of the editor area. Keep the pane
    // at least minOutputRows and leave at least an editor's minimum height
    // (minWinSize.y = 6) above, so the editor can actually shrink that far --
    // otherwise locate() clamps it to its minimum and it overlaps (hides) the
    // pane's top border.
    int h = (outputPaneHeight > 0) ? outputPaneHeight
                                   : max(minOutputRows, totalH / 5);
    h = max((int) minOutputRows, min(h, totalH - 6));
    TRect r = ext;
    r.a.y = ext.b.y - h;           // bottom slice
    // Span only the editor area: stop at the file tree's edge when it is shown.
    if (docTree && (docTree->state & sfVisible))
    {
        TRect t = docTree->getBounds();
        if (t.b.x >= ext.b.x)      // tree docked on the right
            r.b.x = t.a.x;
        else if (t.a.x <= ext.a.x) // tree docked on the left
            r.a.x = t.b.x;
    }
    return r;
}

void TurboApp::showOutput()
{
    if (outputWin && !(outputWin->state & sfVisible))
        toggleOutputView();
}

void TurboApp::toggleOutputView()
{
    if (!outputWin)
        return;
    // Suppress per-editor redraws while we relocate them; draw once at the end.
    MRUlist.forEach([&] (auto *win) { win->setState(sfExposed, False); });
    if (outputWin->state & sfVisible)
    {
        TRect orc = outputWin->getBounds();
        int paneH = orc.b.y - orc.a.y;
        outputWin->hide();
        MRUlist.forEach([&] (auto *win) {
            TRect r = win->getBounds();
            if (r.b.y <= orc.a.y)  // editor sat above the pane: grow back down
                r.b.y += paneH;
            win->locate(r);
        });
    }
    else
    {
        TRect orc = outputBounds();
        outputWin->locate(orc);
        MRUlist.forEach([&] (auto *win) {
            TRect r = win->getBounds();
            if (r.b.y > orc.a.y)   // shrink editors to sit above the pane
                r.b.y = orc.a.y;
            win->locate(r);
        });
        outputWin->show();
    }
    MRUlist.forEach([&] (auto *win) { win->setState(sfExposed, True); });
    deskTop->redraw();
}

void TurboApp::setOutputPaneHeight(int h)
{
    TRect ext = deskTop->getExtent();
    int totalH = ext.b.y - ext.a.y;
    // Leave an editor's min height (6) above, and keep the pane's own minimum.
    h = max((int) minOutputRows, min(h, totalH - 6));
    if (h == outputPaneHeight)
        return;
    if (!outputWin || !(outputWin->state & sfVisible))
    {
        outputPaneHeight = h; // remembered for the next time it is shown
        return;
    }
    int oldTop = outputWin->getBounds().a.y;
    outputPaneHeight = h;
    TRect nb = outputBounds();
    int newTop = nb.a.y;
    if (newTop == oldTop)
        return;
    // Move each editor's bottom that was sitting on the pane to the new pane top
    // (shrinking the pane grows them, growing the pane shrinks them).
    MRUlist.forEach([&] (auto *win) { win->setState(sfExposed, False); });
    MRUlist.forEach([&] (auto *win) {
        TRect r = win->getBounds();
        if (r.b.y >= oldTop)
        {
            r.b.y = newTop;
            win->locate(r);
        }
    });
    outputWin->locate(nb);
    MRUlist.forEach([&] (auto *win) { win->setState(sfExposed, True); });
    deskTop->redraw();
}

void TurboApp::runInOutput(const std::string &label, const std::string &command,
                           std::function<void(int)> onDone)
{
    showOutput();
    if (!outputWin)
        return;
    // BUILD tab: wiped at the start of each run, then shown.
    outputWin->view->clear(otBuild);
    outputWin->showTextTab(otBuild);
    outputWin->view->addLine(otBuild, {"$ " + label, okInfo, "", -1});

    if (buildRunner)
        buildRunner->stop();
    buildRunner = std::make_unique<CommandRunner>();
    buildRunner->onLine = [this] (std::string_view line) {
        if (outputWin)
            outputWin->view->addLine(otBuild,
                parseBuildLine(std::string(line), projectRoot));
    };
    buildRunner->onExit = [this, onDone] (int code) {
        if (outputWin)
            outputWin->view->addLine(otBuild,
                { code == 0 ? std::string("[finished]")
                            : ("[exited with code " + std::to_string(code) + "]"),
                  okInfo, "", -1 });
        if (onDone)
            onDone(code);
    };
    std::string cwd = projectRoot.empty() ? std::string(".") : projectRoot;
    if (!buildRunner->start(command, cwd))
        outputWin->view->addLine(otBuild, {"Failed to start command.", okError, "", -1});
}

void TurboApp::reportGitOutput(const std::string &label, int code,
                               const std::string &output)
{
    showOutput();
    if (!outputWin)
        return;
    // GIT tab: a continuous log (never auto-cleared). Show it and echo the
    // command like a shell prompt, then the captured output verbatim.
    outputWin->showTextTab(otGit);
    outputWin->view->addLine(otGit, {"$ " + label, okInfo, "", -1});
    size_t start = 0;
    while (start < output.size())
    {
        size_t nl = output.find('\n', start);
        size_t end = (nl == std::string::npos) ? output.size() : nl;
        // Drop a trailing '\r' so CRLF output doesn't leave stray carriage returns.
        size_t len = end - start;
        if (len && output[start + len - 1] == '\r')
            --len;
        outputWin->view->addLine(otGit, {output.substr(start, len),
                                  code == 0 ? okNormal : okError, "", -1});
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    if (code != 0)
        outputWin->view->addLine(otGit,
            {"[" + label + " exited with code " + std::to_string(code) + "]",
             okError, "", -1});
    else if (output.empty())
        // A silent success (e.g. `git add`) would otherwise show only the header;
        // confirm it ran so the pane doesn't look like nothing happened.
        outputWin->view->addLine(otGit, {"[done]", okInfo, "", -1});
}

void TurboApp::runBuild()
{
    std::string command = buildConfig.build;
    if (command.empty())
    {
        // No build command configured yet: prompt for a one-off (configure a
        // default in Run > Configure to skip this).
        char cmd[256] = ""; // inputBox's length limit is a uchar (<= 255)
        strnzcpy(cmd, lastBuildCommand.c_str(), sizeof cmd);
        if (inputBox("Build", "Build ~c~ommand:", cmd, sizeof(cmd) - 1) != cmOK)
            return;
        command = trimmed(cmd);
        if (command.empty())
            return;
        lastBuildCommand = command;
    }
    runInOutput(command, command);
}

void TurboApp::editBuildConfig()
{
    if (executeBuildDialog(buildConfig))
        buildConfig.save(projectRoot);
}

void TurboApp::runTest()
{
    if (buildConfig.test.empty())
    {
        messageBox("No test command configured.\nSet one in Run > Configure.",
                   mfInformation | mfOKButton);
        return;
    }
    runInOutput(buildConfig.test, buildConfig.test);
}

bool TurboApp::isArtifactStale() const
{
    namespace fs = std::filesystem;
    if (buildConfig.artifact.empty())
        return true; // no artifact configured -> can't tell, treat as stale
    std::error_code ec;
    fs::path art = fs::path(projectRoot) / buildConfig.artifact;
    if (!fs::exists(art, ec))
        return true; // not built yet
    auto artTime = fs::last_write_time(art, ec);
    if (ec)
        return true;
    // Stale if any project source is newer than the artifact. Always skip the
    // artifact file itself, and -- when the artifact lives in a subdirectory
    // (a build/ dir) -- skip that whole directory so the build's own outputs
    // don't count as "sources". If the artifact sits in the project root, only
    // the artifact file is skipped.
    std::string root = fs::path(projectRoot).lexically_normal().string();
    std::string artFile = art.lexically_normal().string();
    std::string artDir = art.parent_path().lexically_normal().string();
    std::string dirPrefix;
    if (artDir.size() > root.size())
        dirPrefix = artDir + "/";
    std::vector<std::string> files;
    if (docTree && docTree->tree)
        docTree->tree->collectFilePaths(files);
    for (auto &f : files)
    {
        std::string fn = fs::path(f).lexically_normal().string();
        if (fn == artFile)
            continue;
        if (!dirPrefix.empty() && fn.rfind(dirPrefix, 0) == 0)
            continue;
        auto t = fs::last_write_time(fs::path(f), ec);
        if (!ec && t > artTime)
            return true;
    }
    return false;
}

bool TurboApp::needsBuildBeforeRun() const
{
    if (buildConfig.runMode == "run")
        return false;
    if (buildConfig.runMode == "build")
        return true;
    return isArtifactStale(); // "auto": build only when the artifact is stale
}

void TurboApp::runRun()
{
    if (buildConfig.run.empty())
    {
        messageBox("No run command configured.\nSet one in Run > Configure.",
                   mfInformation | mfOKButton);
        return;
    }
    if (needsBuildBeforeRun() && !buildConfig.build.empty())
    {
        // Build first; on success, start Run on the next idle tick (so we don't
        // tear down buildRunner from inside its own onExit/pump).
        runInOutput(buildConfig.build, buildConfig.build, [this] (int code) {
            if (code == 0)
                pendingAfterBuild = [this] { startRun(); };
            else if (outputWin)
                outputWin->view->addLine(otBuild, {"[build failed -- not running]", okError, "", -1});
        });
    }
    else
        startRun();
}

void TurboApp::startRun()
{
    // Tool processes are managed independently now (Run-menu toggles); Run only
    // runs the configured run command and streams it into the pane.
    runInOutput(buildConfig.run, buildConfig.run);
}

// ---------------------------------------------------------------------------
// Tool processes (Run-menu toggles)

void TurboApp::editToolsConfig()
{
    if (executeToolsDialog(buildConfig.extra))
    {
        buildConfig.save(projectRoot);
        applyToolConfig();
    }
}

bool TurboApp::toolRunning(int i) const noexcept
{
    return i >= 0 && i < (int) tools.size() && tools[i].runner &&
           tools[i].runner->running();
}

void TurboApp::applyToolConfig() noexcept
{
    // Reconcile 'tools' with buildConfig.extra, preserving a running process (and
    // its Output tab) when its name+command are unchanged. Anything left over was
    // removed or edited: stop it and drop its Output tab.
    std::vector<ToolProcess> next;
    next.reserve(buildConfig.extra.size());
    std::vector<bool> consumed(tools.size(), false);
    for (auto &e : buildConfig.extra)
    {
        int match = -1;
        for (int i = 0; i < (int) tools.size(); ++i)
            if (!consumed[i] && tools[i].name == e.name &&
                tools[i].command == e.command)
            { match = i; break; }
        if (match >= 0)
        {
            consumed[match] = true;
            next.push_back(std::move(tools[match]));
        }
        else
            next.push_back(ToolProcess{ e.name, e.command, -1, nullptr });
    }
    for (int i = 0; i < (int) tools.size(); ++i)
        if (!consumed[i])
        {
            if (tools[i].runner)
                tools[i].runner->stop();
            if (outputWin && tools[i].tabId >= 0)
                outputWin->view->removeTab(tools[i].tabId);
        }
    tools = std::move(next);

    // Resize / relabel the Run menu's tool section. A count change is otherwise
    // picked up by refreshWindowList (idle); rebuild now so the labels are right
    // straight away, and relabel in place when only the names changed.
    int toolCount = (int) tools.size();
    if (toolCount > toolListMax)
        toolCount = toolListMax;
    if (toolCount != menuToolCount)
        rebuildMenuBar(menuRecentCount, toolCount);
    else
        fillToolMenuLabels();
}

void TurboApp::fillToolMenuLabels() noexcept
{
    auto *bar = (TurboMenuBar *) menuBar;
    if (!bar)
        return;
    TMenu *m = bar->rootMenu();
    for (int i = 0; i < menuToolCount && i < (int) tools.size(); ++i)
    {
        const ToolProcess &t = tools[i];
        std::string name = t.name.empty() ? t.command : t.name;
        setMenuItemLabel(m, cmToolBase + i, name.c_str(), true);
    }
}

void TurboApp::toggleTool(int i) noexcept
{
    if (i < 0 || i >= (int) tools.size())
        return;
    if (toolRunning(i))
        stopTool(i);
    else
        startTool(i);
    refreshWindowList(); // reflect the new check state immediately
}

void TurboApp::startTool(int i) noexcept
{
    if (i < 0 || i >= (int) tools.size() || tools[i].command.empty())
        return;
    ToolProcess &t = tools[i];
    std::string cwd = projectRoot.empty() ? std::string(".") : projectRoot;
    std::string title = t.name.empty() ? t.command : t.name;
    if (t.tabId < 0)
        t.tabId = nextToolTabId++; // allocate a stable Output tab on first start
    int tab = t.tabId;
    showOutput();
    if (outputWin)
    {
        outputWin->view->ensureTab(tab, title);
        outputWin->view->clear(tab); // fresh run: wipe the previous output
        outputWin->view->addLine(tab, {"$ " + t.command, okInfo, "", -1});
        outputWin->showTextTab(tab);
    }
    t.runner = std::make_unique<CommandRunner>();
    t.runner->onLine = [this, tab] (std::string_view line) {
        if (outputWin)
            outputWin->view->addLine(tab,
                parseBuildLine(std::string(line), projectRoot));
    };
    t.runner->onExit = [this, tab] (int code) {
        if (outputWin)
            outputWin->view->addLine(tab,
                { code == 0 ? std::string("[finished]")
                            : ("[exited with code " + std::to_string(code) + "]"),
                  okInfo, "", -1 });
        // running() is already false; refreshWindowList (idle) clears the check.
    };
    if (!t.runner->start(t.command, cwd))
    {
        if (outputWin)
            outputWin->view->addLine(tab, {"Failed to start tool.", okError, "", -1});
        t.runner.reset();
    }
}

// --- Debugger (Debug Adapter Protocol) --------------------------------------

// Maps Turbo's detected Language to a DAP adapter/language id. Returns "" when
// no debug adapter is wired for the language (mirrors LSP's languageIdFor).
static std::string dapLanguageId(const turbo::Language *lang) noexcept
{
    using turbo::Language;
    if (!lang) return {};
    if (lang == &Language::CPP)    return "cpp";
    if (lang == &Language::Python) return "python";
    if (lang == &Language::PHP)    return "php";
    return {};
}

void TurboApp::initDap() noexcept
{
    dap = std::make_unique<DapManager>();
    // Wake the event loop so pump() delivers adapter events (notably 'stopped')
    // promptly, without waiting for the next keystroke.
    dap->onWake = [] { TEventQueue::wakeUp(); };
    dap->onOutput = [this] (const std::string &category, const std::string &text) {
        debugConsoleOutput(category, text);
    };
    dap->onSessionState = [this] (bool active) {
        if (outputWin && debugConsoleTab >= 0)
            outputWin->view->addLine(debugConsoleTab,
                { active ? std::string("[debug session started]")
                         : std::string("[debug session ended]"),
                  okInfo, "", -1 });
        if (outputWin)
            outputWin->setDebugTabsVisible(active); // add/remove the Call Stack tab
        if (!active)
        {
            clearDebugCurrentLine();
            if (outputWin && outputWin->stackView)
                outputWin->stackView->clearFrames();
            if (outputWin && outputWin->varsView)
                outputWin->varsView->clearTree();
        }
    };
    dap->onStopped = [this] (const std::string &file, int line, const std::string &) {
        clearDebugCurrentLine();
        if (!file.empty() && line > 0)
        {
            openOrFocus(file, line - 1); // DAP 1-based -> editor 0-based
            if (auto *w = focusedEditor())
                w->getEditor().setCurrentLine(line - 1);
        }
    };
    dap->onFrames = [this] (const std::vector<StackFrameInfo> &frames) {
        if (!outputWin || !outputWin->stackView)
            return;
        std::vector<StackFrameItem> items;
        items.reserve(frames.size());
        for (const StackFrameInfo &f : frames)
        {
            std::string loc;
            if (!f.file.empty())
            {
                std::string base = f.file;
                auto slash = base.find_last_of("/\\");
                if (slash != std::string::npos)
                    base = base.substr(slash + 1);
                loc = "  " + base + ":" + std::to_string(f.line);
            }
            StackFrameItem it;
            it.label = f.name + loc;
            it.file = f.file;
            it.line = f.line > 0 ? f.line - 1 : -1; // 0-based for jump
            it.frameId = f.id;
            items.push_back(std::move(it));
        }
        outputWin->stackView->setFrames(std::move(items));
    };
    dap->onScopes = [this] (const std::vector<ScopeInfo> &scopes) {
        if (!outputWin || !outputWin->varsView)
            return;
        std::vector<VarItem> items;
        items.reserve(scopes.size());
        for (const ScopeInfo &s : scopes)
            items.push_back(VarItem{s.name, s.variablesReference});
        outputWin->varsView->setScopes(items);
    };
    dap->onContinued = [this] {
        clearDebugCurrentLine();
        if (outputWin && outputWin->stackView)
            outputWin->stackView->clearFrames();
        if (outputWin && outputWin->varsView)
            outputWin->varsView->clearTree();
    };
}

void TurboApp::startDebug()
{
    if (!dap)
        return;
    EditorWindow *w = focusedEditor();
    if (!w)
    {
        messageBox("Open a file to debug first.", mfInformation | mfOKButton);
        return;
    }
    std::string path = w->filePath();
    if (path.empty())
    {
        messageBox("Save the file before starting the debugger.", mfInformation | mfOKButton);
        return;
    }
    std::string lang = dapLanguageId(w->getEditor().language);
    if (lang.empty())
    {
        messageBox("No debug adapter is available for this file type.",
                   mfInformation | mfOKButton);
        return;
    }
    // Prepare (and reveal) the Debug Console tab for the debuggee's stdio.
    if (outputWin)
    {
        if (debugConsoleTab < 0)
            debugConsoleTab = nextToolTabId++;
        outputWin->view->ensureTab(debugConsoleTab, "DEBUG");
        outputWin->view->clear(debugConsoleTab);
        showOutput();
        outputWin->showTextTab(debugConsoleTab);
    }
    dap->start(lang, path); // failures are reported via onOutput
}

void TurboApp::stopDebug()
{
    if (dap)
        dap->terminate();
}

void TurboApp::debugConsoleOutput(const std::string &category, const std::string &text) noexcept
{
    if (!outputWin || debugConsoleTab < 0)
        return;
    OutputKind kind = (category == "stderr") ? okError
                    : (category == "important" || category == "console") ? okInfo
                    : okNormal;
    // The adapter batches output arbitrarily; split into lines for the list.
    size_t start = 0;
    while (start < text.size())
    {
        size_t nl = text.find('\n', start);
        size_t end = (nl == std::string::npos) ? text.size() : nl;
        outputWin->view->addLine(debugConsoleTab, {text.substr(start, end - start), kind, "", -1});
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    showOutput();
}

void TurboApp::editorToggleBreakpoint(EditorWindow &w, long line) noexcept
{
    if (!dap)
        return;
    std::string file = w.filePath();
    if (file.empty())
        return; // no breakpoints on unsaved scratch buffers
    bool on = dap->toggleBreakpoint(file, (int) line);
    w.getEditor().setBreakpointMarker(line, on);
}

void TurboApp::clearDebugCurrentLine() noexcept
{
    MRUlist.forEach([] (EditorWindow *w) {
        if (w)
            w->getEditor().setCurrentLine(-1);
    });
}

void TurboApp::stopTool(int i) noexcept
{
    if (i < 0 || i >= (int) tools.size() || !tools[i].runner)
        return;
    tools[i].runner->stop(); // no onExit fires; keep the tab + its output
    tools[i].runner.reset();
    if (outputWin && tools[i].tabId >= 0)
        outputWin->view->addLine(tools[i].tabId, {"[stopped]", okInfo, "", -1});
}

void TurboApp::stopAllTools() noexcept
{
    for (auto &t : tools)
        if (t.runner)
        {
            t.runner->stop();
            t.runner.reset();
        }
}

void TurboApp::stopAll()
{
    // Stop only the current Build/Run/Test command; tool processes are long-lived
    // and toggled off individually from the Run menu.
    bool had = buildRunner && buildRunner->running();
    pendingAfterBuild = nullptr;
    if (buildRunner)
        buildRunner->stop();
    if (outputWin && had)
        outputWin->view->addLine(otBuild, {"[stopped]", okInfo, "", -1});
}

void TurboApp::newTerminal()
{
    // Open over the editor area (to the right of the file tree if it is shown),
    // like a new editor window; the user can then zoom/move/resize it as usual.
    TRect r = deskTop->getExtent();
    if (docTree && (docTree->state & sfVisible))
    {
        TRect t = docTree->getBounds();
        if (t.a.x > r.b.x - t.b.x)
            r.b.x = max(t.a.x, 20);
        else
            r.a.x = min(t.b.x, r.b.x - 20);
    }
    auto *win = new TerminalWindow(r);
    deskTop->insert(win);
}

void TurboApp::newSpec()
{
    if (projectRoot.empty())
    {
        messageBox(mfError | mfOKButton,
                   "Open a project first: specs live in its specs/ directory.");
        return;
    }
    std::string title, domain, goal;
    if (!executeNewSpecDialog(title, domain, goal))
        return;
    std::string stem = turbo::kebabCase(title);
    if (stem.empty())
    {
        messageBox(mfError | mfOKButton,
                   "The title needs at least one letter or digit.");
        return;
    }
    std::error_code ec;
    std::string dir = projectRoot + "/specs";
    std::filesystem::create_directories(dir, ec);
    std::string path = dir + "/" + stem + ".md";
    if (std::filesystem::exists(path, ec))
    {
        // The spec already exists: editing it beats refusing or clobbering.
        openOrFocus(path);
        return;
    }
    char date[16];
    std::time_t now = std::time(nullptr);
    std::strftime(date, sizeof date, "%Y-%m-%d", std::localtime(&now));
    {
        std::ofstream f(path, std::ios::binary);
        if (!f)
        {
            messageBox(mfError | mfOKButton, "Cannot create '%s'.", path.c_str());
            return;
        }
        f << turbo::specTemplate(title, domain, goal, date);
    }
    openOrFocus(path);
}

void TurboApp::toggleAgent()
{
    if (agentWin)
    {
        // Already open: bring it to the front and give it focus.
        agentWin->focus();
        return;
    }
    std::string cmd = resolveAgentCommand(buildConfig.agent, settings.defaultAgent);
    if (cmd.empty())
    {
        messageBox("No coding agent is configured.", mfInformation | mfOKButton);
        return;
    }
    // Open a normal, freely-placeable window over the editor area (to the right
    // of the file tree if shown), like a new terminal; the user can zoom/move/
    // resize it. The back-pointer nulls agentWin when the window closes.
    TRect r = deskTop->getExtent();
    if (docTree && (docTree->state & sfVisible))
    {
        TRect t = docTree->getBounds();
        if (t.a.x > r.b.x - t.b.x)
            r.b.x = max(t.a.x, 20);
        else
            r.a.x = min(t.b.x, r.b.x - 20);
    }
    agentWin = new TerminalWindow(r, cmd, "Agent (" + cmd + ")", &agentWin);
    deskTop->insert(agentWin);
}

void TurboApp::selectAgent()
{
    char buf[256] = {};
    std::string current = !buildConfig.agent.empty() ? buildConfig.agent
                                                      : settings.defaultAgent;
    strncpy(buf, current.c_str(), sizeof(buf) - 1);
    if (inputBox("Select Agent",
                 "Agent (claude / codex / opencode, or a command):",
                 buf, sizeof(buf) - 1) != cmOK)
        return;
    if (hasProject())
    {
        buildConfig.agent = buf;
        buildConfig.save(projectRoot); // .turbo/config.json
    }
    else
    {
        settings.defaultAgent = buf;   // no project: set the global default
        saveSettings(settings);
    }
    messageBox(mfInformation | mfOKButton,
               "Agent set to '%s'.%s", resolveAgentCommand(buildConfig.agent,
                                                            settings.defaultAgent).c_str(),
               agentWin ? " Use Restart Agent to apply." : "");
}

void TurboApp::restartAgent()
{
    if (agentWin)
        agentWin->close(); // shutDown() nulls agentWin as it is destroyed
    toggleAgent();
}

void TurboApp::registerTerminal(TerminalView *t) noexcept
{
    terminals.push_back(t);
}

void TurboApp::unregisterTerminal(TerminalView *t) noexcept
{
    for (auto it = terminals.begin(); it != terminals.end(); ++it)
        if (*it == t)
        {
            terminals.erase(it);
            break;
        }
}

void TurboApp::handleFocus(EditorWindow &w) noexcept
{
    // w has been focused, so it becomes the first of our MRU list.
    w.listHead.insert_after(&MRUlist);
    if (docTree)
        docTree->tree->focusEditor(&w);
    // We keep track of the most recent directory for file dialogs.
    if (!w.filePath().empty())
        mostRecentDir = TPath::dirname(w.filePath());
}

void TurboApp::handleTitleChange(EditorWindow &w) noexcept
{
    auto &counter = fileCount[TPath::basename(w.filePath())];
    bool renamed = (&counter != w.fileNumber.counter);
    if (renamed)
        w.fileNumber = {counter};
    if (docTree)
    {
        if (renamed)
        {
            // The file path may have changed (rename): relink to the new node.
            docTree->tree->unlinkEditor(&w);
            docTree->tree->linkEditor(&w);
            docTree->tree->focusEditor(&w);
        }
        else if (auto *node = docTree->tree->findByEditor(&w))
        {
            // Save point reached/lost: refresh the unsaved-changes marker.
            node->refreshText();
            docTree->tree->drawView();
        }
    }
    if (!w.filePath().empty() && w.state & sfActive)
        mostRecentDir = TPath::dirname(w.filePath());
}

void TurboApp::removeEditor(EditorWindow &w) noexcept
{
    // Fire while the window is still valid and linked.
    fireLuaEvent("closeFile", {{"path", w.filePath()}});
    if (lsp)
        lsp->didClose(w);
    w.listHead.remove();
    if (MRUlist.empty())
        disableCommands(editorCmds);
    if (docTree)
    {
        docTree->tree->unlinkEditor(&w);
        // Removing the editor causes the focus to stay on the same position
        // but maybe not on the right element.
        if (!MRUlist.empty())
            docTree->tree->focusEditor(MRUlist.next->self);
    }
}

const char *TurboApp::getFileDialogDir() noexcept
{
    return mostRecentDir.c_str();
}

bool TurboApp::autoSaveOnFocusLoss() noexcept
{
    return settings.autoSaveOnFocusLoss;
}

void TurboApp::editorTextChanged(EditorWindow &w) noexcept
{
    if (lsp)
        lsp->didChange(w);
}

void TurboApp::editorWillSave(EditorWindow &w) noexcept
{
    fireLuaEvent("beforeSave", {{"path", w.filePath()}});
}

void TurboApp::editorSaved(EditorWindow &w) noexcept
{
    // Re-baseline before the watcher can report our own write, so this save
    // isn't mistaken for an external change.
    rememberDiskSignature(w);
    if (lsp)
        lsp->didSave(w);
    if (git)
        git->requestStatus(); // a save may change the file's git status
    fireLuaEvent("afterSave", {{"path", w.filePath()}});
}

void TurboApp::editorCharAdded(EditorWindow &w, int ch) noexcept
{
    if (lsp)
        lsp->charAdded(w, ch);
}

void TurboApp::editorRequestCompletion(EditorWindow &w) noexcept
{
    if (lsp)
        lsp->requestCompletion(w);
}

void TurboApp::editorHoverStart(EditorWindow &w, long pos) noexcept
{
    if (lsp)
        lsp->hover(w, pos);
}

void TurboApp::editorHoverEnd(EditorWindow &w) noexcept
{
    if (lsp)
        lsp->hoverEnd(w);
}
