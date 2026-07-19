#define Uses_TRect
#define Uses_TProgram
#define Uses_TDeskTop
#define Uses_TObject
// Pull in <tvision/editors.h> for the cmFind/cmReplace/cmSearchAgain command
// constants referenced in the table below, so this file does not depend on a
// unity-build batch-mate having included editors.h first.
#define __INC_EDITORS_H
#include <tvision/tv.h>

#include "commandpalette.h"
#include "fuzzypicker.h"
#include "fuzzymatch.h"
#include "cmds.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

struct PaletteCommand
{
    const char *label;
    ushort command;
    const char *shortcut; // display hint (may be "")
    bool needsEditor;     // only meaningful with an open editor
};

// The palette's command surface, mirroring the menu bar (app.cc initMenuBar).
// Keeping it here as one table doubles as a readable index of what the app can
// do; new menu commands should be added here too.
const PaletteCommand kCommands[] =
{
    // File
    { "New File",                      cmNew,                  "Ctrl-N",       false },
    { "Open File...",                  cmOpen,                 "Ctrl-O",       false },
    { "Open Directory...",             cmOpenDir,              "",             false },
    { "Close Project",                 cmCloseProject,         "",             false },
    { "Save",                          cmSave,                 "Ctrl-S",       true  },
    { "Save As...",                    cmSaveAs,               "",             true  },
    { "Rename...",                     cmRename,               "F2",           true  },
    { "Close Editor",                  cmCloseEditor,          "Ctrl-W",       true  },
    { "Close All",                     cmCloseAll,             "",             true  },
    { "New Terminal",                  cmNewTerminal,          "",             false },
    { "Toggle Agent",                  cmToggleAgent,          "Alt-0",        false },
    { "Restart Agent",                 cmRestartAgent,         "",             false },
    { "Select Agent...",               cmSelectAgent,          "",             false },
    { "Suspend",                       cmDosShell,             "",             false },
    { "Exit",                          cmQuit,                 "Ctrl-Q",       false },
    // Specs (spec-driven ALM)
    { "New Spec...",                   cmNewSpec,              "",             false },
    // Navigation (new)
    { "Go to Anything...",             cmGotoAnything,         "Ctrl-P",       false },
    // Edit
    { "Undo",                          cmUndo,                 "Ctrl-Z",       true  },
    { "Redo",                          cmRedo,                 "Ctrl-Y",       true  },
    { "Cut",                           cmCut,                  "Ctrl-X",       true  },
    { "Copy",                          cmCopy,                 "Ctrl-C",       true  },
    { "Paste",                         cmPaste,                "Ctrl-V",       true  },
    { "Find...",                       cmFind,                 "Ctrl-F",       true  },
    { "Replace...",                    cmReplace,              "Ctrl-R",       true  },
    { "Go to Line...",                 cmGoToLine,             "Ctrl-G",       true  },
    { "Find Next",                     cmSearchAgain,          "F3",           true  },
    { "Find Previous",                 cmSearchPrev,           "Shift-F3",     true  },
    { "Complete",                      cmCompletion,           "Ctrl-Space",   true  },
    // Selection / multi-cursor
    { "Toggle Comment",                cmToggleComment,        "Ctrl-E",       true  },
    { "Uppercase Selection",           cmSelUppercase,         "",             true  },
    { "Lowercase Selection",           cmSelLowercase,         "",             true  },
    { "Capitalize Selection",          cmSelCapitalize,        "",             true  },
    { "Select Next Occurrence",        cmSelectNextOccurrence, "Ctrl-D",       true  },
    { "Select All Occurrences",        cmSelectAllOccurrences, "",             true  },
    { "Add Caret Up",                  cmAddCaretUp,           "Ctrl-Alt-Up",  true  },
    { "Add Caret Down",                cmAddCaretDown,         "Ctrl-Alt-Down",true  },
    { "Split Selection into Lines",    cmSplitSelectionLines,  "",             true  },
    { "Skip Occurrence",               cmSkipOccurrence,       "",             true  },
    { "Undo Last Selection",           cmUndoSelection,        "Ctrl-U",       true  },
    // Code
    { "Toggle Code Folding",           cmToggleFolding,        "",             true  },
    { "Toggle Fold at Cursor",         cmFoldAtCursor,         "",             true  },
    { "Fold All",                      cmFoldAll,              "",             true  },
    { "Unfold All",                    cmUnfoldAll,            "",             true  },
    { "Toggle Bookmark",               cmToggleBookmark,       "",             true  },
    { "Next Bookmark",                 cmNextBookmark,         "",             true  },
    { "Previous Bookmark",             cmPrevBookmark,         "",             true  },
    // Settings
    { "Toggle Line Numbers",           cmToggleLineNums,       "F8",           true  },
    { "Toggle Line Wrapping",          cmToggleWrap,           "F9",           true  },
    { "Toggle Auto Indent",            cmToggleIndent,         "",             true  },
    { "Toggle File Tree View",         cmToggleTree,           "",             false },
    { "Show Hidden Files",             cmToggleHidden,         "",             false },
    { "Toggle Auto-save on Focus Loss",cmToggleAutoSave,       "",             false },
    { "Toggle Change History",         cmToggleChangeHistory,  "",             true  },
    { "Toggle Long Line Guide",        cmToggleEdge,           "",             true  },
    { "Colour Scheme...",              cmThemeSettings,        "",             false },
    { "Colour Mode: Auto",             cmColorModeAuto,        "",             false },
    { "Colour Mode: Full (24-bit)",    cmColorModeFull,        "",             false },
    { "Colour Mode: 16-colour (classic)", cmColorMode16,       "",             false },
    { "Tree Icons: Auto",              cmTreeIconsAuto,        "",             false },
    { "Tree Icons: Nerd Font",         cmTreeIconsNerd,        "",             false },
    { "Tree Icons: Unicode",           cmTreeIconsUnicode,     "",             false },
    { "Tree Icons: ASCII",             cmTreeIconsAscii,       "",             false },
    { "Language Servers...",           cmLspSettings,          "",             false },
    { "Debuggers...",                  cmDebugSettings,        "",             false },
    // Build / Run
    { "Build",                         cmBuild,                "F7",           false },
    { "Run",                           cmRun,                  "",             false },
    { "Test",                          cmTest,                 "",             false },
    { "Stop",                          cmStop,                 "",             false },
    { "Build: Configure...",           cmBuildConfig,          "",             false },
    { "Tools: Configure...",           cmToolsConfig,          "",             false },
    { "Toggle Output Pane",            cmToggleOutput,         "",             false },
    // Debug
    { "Debug: Start",                  cmDebugStart,           "",             true  },
    { "Debug: Stop",                   cmDebugStop,            "",             false },
    { "Debug: Continue",               cmDebugContinue,        "",             false },
    { "Debug: Step Over",              cmDebugStepOver,        "",             false },
    { "Debug: Step Into",              cmDebugStepInto,        "",             false },
    { "Debug: Step Out",               cmDebugStepOut,         "",             false },
    { "Debug: Call Stack",             cmDebugCallStack,       "",             false },
    { "Debug: Variables",              cmDebugVariables,       "",             false },
    { "Debug: Toggle Breakpoint",      cmToggleBreakpoint,     "",             true  },
    // Git
    { "Git: Commit...",                cmGitCommit,            "",             false },
    { "Git: Fetch",                    cmGitFetch,             "",             false },
    { "Git: Pull",                     cmGitPull,              "",             false },
    { "Git: Push",                     cmGitPush,              "",             false },
    { "Git: New Branch...",            cmGitNewBranch,         "",             false },
    { "Git: Refresh Status",           cmGitRefresh,           "",             false },
    // Lua
    { "Lua: Run Script...",            cmLuaRunScript,         "",             false },
    { "Lua: New Script...",            cmLuaNewScript,         "",             false },
    { "Lua: Reload Config",            cmLuaReload,            "",             false },
    // Windows
    { "Zoom Window",                   cmZoom,                 "F5",           true  },
    { "Resize / Move Window",          cmResize,               "Ctrl-F5",      true  },
    { "Next Editor",                   cmEditorNext,           "F6",           true  },
    { "Previous Editor",               cmEditorPrev,           "Shift-F6",     true  },
    // Help
    { "Keyboard Shortcuts",            cmHelp,                 "F1",           false },
    { "About",                         cmAbout,                "",             false },
};

} // namespace

ushort runCommandPalette(bool hasEditor,
                         const std::vector<PaletteExtra> &extra) noexcept
{
    auto provider = [hasEditor, &extra] (const std::string &query) -> std::vector<FuzzyPicker::Row>
    {
        // A scored entry references either a static command (idx >= 0) or a
        // dynamic extra (idx < 0, encoding -(extraIndex + 1)).
        struct Scored { int idx; int score; bool avail; };
        std::vector<Scored> scored;
        int count = (int) (sizeof(kCommands) / sizeof(kCommands[0]));
        for (int i = 0; i < count; ++i)
        {
            int s = 0;
            if (!fuzzy::fuzzyScore(query, kCommands[i].label, &s))
                continue;
            bool avail = !kCommands[i].needsEditor || hasEditor;
            // Unavailable commands sink below every available one.
            scored.push_back({ i, avail ? s : s - 1000000, avail });
        }
        for (int e = 0; e < (int) extra.size(); ++e)
        {
            int s = 0;
            if (!fuzzy::fuzzyScore(query, extra[e].label.c_str(), &s))
                continue;
            scored.push_back({ -(e + 1), s, true });
        }
        std::sort(scored.begin(), scored.end(),
                  [] (const Scored &a, const Scored &b) { return a.score > b.score; });
        std::vector<FuzzyPicker::Row> out;
        out.reserve(scored.size());
        for (auto &sc : scored)
        {
            if (sc.idx >= 0)
            {
                const PaletteCommand &c = kCommands[sc.idx];
                out.push_back({ c.label, c.shortcut ? c.shortcut : "",
                                (int) c.command, !sc.avail });
            }
            else
            {
                const PaletteExtra &x = extra[-sc.idx - 1];
                out.push_back({ x.label.c_str(), x.detail.c_str(),
                                (int) x.command, false });
            }
        }
        return out;
    };

    TRect ext = TProgram::deskTop->getExtent();
    int w = std::min(76, (int) ext.b.x - 4);
    int h = std::min(20, (int) ext.b.y - 4);
    TRect r(0, 0, w, h);
    r.move((ext.b.x - w) / 2, (ext.b.y - h) / 3);

    auto *picker = new FuzzyPicker(r, "Command Palette", provider, /*withPreview=*/false);
    int payload = picker->run();
    TObject::destroy(picker);
    return payload < 0 ? 0 : (ushort) payload;
}
