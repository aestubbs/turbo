#ifndef TURBO_APP_H
#define TURBO_APP_H

#define Uses_TApplication
#define Uses_TFileDialog
#define Uses_TMenuBar
#define Uses_TMenu
#include <tvision/tv.h>

#include <memory>
#include <vector>
#include <fstream>
#include <functional>

#include <turbo/editstates.h>
#include <turbo/filewatcher.h>
#include "doctree.h"
#include "apputils.h"
#include "editwindow.h"
#include "settings.h"
#include "frecency.h"
#include "outputwindow.h"
#include "commandrunner.h"
#include "buildconfig.h"
#include "debugconfig.h"
#include "cmds.h"

struct EditorWindow;
class TClockView;
class LspManager;
class DapManager;
class GitManager;
class LuaManager;
class McpServer;
struct BranchView;
struct TerminalView;
struct TerminalWindow;

// A configured tool process, toggled on/off from the Run menu (e.g. `npm run
// dev`). Independent of Build/Run: it runs long-lived until toggled off, and its
// combined output streams into its own tab in the Output pane. 'runner' is
// non-null only while it is running; 'tabId' is its Output-pane tab, allocated
// lazily on first start (-1 until then).
struct ToolProcess
{
    std::string name;
    std::string command;
    int tabId {-1};
    std::unique_ptr<CommandRunner> runner;
};

// TMenuView::menu (the root of the menu tree) is protected; expose it so the app
// can rewrite toggle-item labels to show check marks.
struct TurboMenuBar : public TMenuBar
{
    using TMenuBar::TMenuBar;
    TMenu *rootMenu() const noexcept { return menu; }
};

struct TurboApp : public TApplication, EditorWindowParent
{

    FileCounter fileCount;
    list_head<EditorWindow> MRUlist;
    TClockView *clock;
    DocumentTreeWindow *docTree;
    TCommandSet editorCmds;
    // Number of recent-window slots currently built into the Windows menu. The
    // menu bar is rebuilt when this needs to change, so there are never empty
    // placeholder rows (0 open editors = no slots).
    int menuRecentCount {0};
    // Which of those slots holds the agent window, or -1 while it is closed. It
    // is not an editor -- it never joins MRUlist -- so it gets a slot of its own
    // after the editors, and focusRecentWindow has to route that index to it.
    int menuAgentIndex {-1};
    bool argsParsed {false};
    int argc;
    const char **argv;
    turbo::SearchSettings searchSettings;
    std::string mostRecentDir;
    AppSettings settings;
    FrecencyStore frecency;
    std::unique_ptr<LspManager> lsp;
    // Debug Adapter Protocol session manager (debugger integration). Owns at
    // most one live debug session; pumped each idle tick like lsp.
    std::unique_ptr<DapManager> dap;
    std::unique_ptr<GitManager> git;
    std::unique_ptr<turbo::FileWatcher> watcher;
    // The global skills home (~/.claude/skills) lives outside the project, so the
    // project watcher -- which is rooted at projectRoot -- cannot see it. It gets
    // its own, which stays up across project open/close since it isn't project-scoped.
    std::unique_ptr<turbo::FileWatcher> skillsWatcher;
    std::string globalSkillsDir;    // "" when there is no $HOME
    // Embedded Lua interpreter: editor scripting/configuration and event hooks.
    std::unique_ptr<LuaManager> luaMgr;
    // MCP server: exposes editor actions + Lua commands to the agent as tools.
    std::unique_ptr<McpServer> mcp;

    // Branch indicator at the right of the menu bar (clickable; opens a popup of
    // the other branches). 'branchTextShown' is the last text written to it, so
    // the idle refresh only repaints/resizes on change.
    BranchView *branchView {nullptr};
    std::string branchTextShown;

    // Open terminal views, pumped each idle tick (see newTerminal()).
    std::vector<TerminalView *> terminals;

    // The dedicated coding-agent window (a normal, freely-placeable terminal
    // window running the configured agent). Single instance; nulled on close.
    TerminalWindow *agentWin {nullptr};

    // Build/Run: a bordered output pane docked at the bottom of the editor area,
    // and the command runner streaming the current build into it.
    OutputWindow *outputWin {nullptr};
    int outputPaneHeight {0}; // user-set pane height in rows (0 = ~1/5 default)
    std::unique_ptr<CommandRunner> buildRunner;
    std::string projectRoot;      // cwd the app was opened from (build cwd)
    std::string lastBuildCommand; // remembered between Build invocations
    BuildConfig buildConfig;      // .turbo/config.json (build/test/run + tools)
    DebugConfig debugConfig;      // .turbo/debug.json (per-language debug adapters)
    // Configured tool processes, mirroring buildConfig.extra. Toggled on/off from
    // the Run menu, pumped each idle tick, each streaming to its own Output tab.
    std::vector<ToolProcess> tools;
    int nextToolTabId {2};        // Output-pane tab-id allocator (0/1 = BUILD/GIT)
    int debugConsoleTab {-1};     // Output-pane tab for the debuggee's stdio (-1 = none yet)
    int menuToolCount {0};        // tool-toggle slots currently built into the menu
    // Deferred action run on the next idle tick (used to start Run after a
    // build-first finishes, so we don't reassign buildRunner inside its pump).
    std::function<void()> pendingAfterBuild;

    TurboApp(int argc, const char **argv) noexcept;
    ~TurboApp();
    static TMenuBar* initMenuBar(TRect r);
    // Build the menu bar with 'recentCount' recent-window slots in the Windows
    // menu and 'toolCount' tool-toggle slots in the Run menu (initMenuBar builds
    // with 0/0; rebuildMenuBar swaps in a new bar when either count changes).
    static TMenuBar* makeMenuBar(TRect r, int recentCount, int toolCount);
    void rebuildMenuBar(int recentCount, int toolCount);
    static TStatusLine* initStatusLine(TRect r);
    // Dark, high-contrast palette for the menu bar and status line (indices 2..7
    // of the application palette), so disabled items stay readable.
    TPalette &getPalette() const override;
    // Custom desktop: a solid colour fill instead of Turbo Vision's shaded
    // (\xB0) pattern background.
    static TDeskTop* initDeskTop(TRect r);

    void shutDown() override;
    void idle() override;
    void getEvent(TEvent &event) override;
    void handleEvent(TEvent& event) override;
    void parseArgs();

    void fileNew();
    // Guided "New File...": prompt for name+location (Save As), then create and
    // open it so the right lexer applies from the start (see cmNewNamedFile).
    void fileNewNamedFile();
    void fileOpen();
    void fileOpenOrNew(const char *path);
    void openFileFromTree(const char *absPath);
    // Focus the editor already showing 'absPath', or open it; then jump to
    // 'line' (0-based; <0 = no jump) and record the open for frecency ranking.
    void openOrFocus(const std::string &absPath, long line = -1) noexcept;
    // Fuzzy navigation overlays (Ctrl-P / Ctrl-Shift-P).
    void gotoAnything();
    void commandPalette();
    // --- Project (workspace) -------------------------------------------------
    // Open 'dir' as the project: scan it into the file tree, point the build
    // system, git, LSP and the filesystem watcher at it, and load its .turbo Lua
    // hooks. Replaces any project already open (the IDE holds one at a time).
    void openProject(const std::string &dir) noexcept;
    // Close the current project: save its session, close every editor whose file
    // lives inside the project directory, empty the file tree, stop the watcher
    // and clear git/LSP/build state. Editors holding files outside the project
    // (or unsaved scratch buffers) are left open. No-op when no project is open.
    void closeProject() noexcept;
    bool hasProject() const noexcept { return !projectRoot.empty(); }
    // Pick a directory (folder chooser) and open it as the project.
    void fileOpenDir();
    // Close every open editor whose file lives inside the current project root.
    void closeProjectEditors() noexcept;
    // Persist / restore the set of open project editors (paths + cursor line +
    // which was active) to <projectRoot>/.turbo/session, so reopening a project
    // brings its windows back. Both are no-ops when no project is open.
    void saveSession() noexcept;
    void restoreSession() noexcept;
    void toggleAutoSave();
    void toggleHiddenFiles();
    // Rewrite the toggle menu items' labels so enabled ones show a check mark.
    // Reflects the active editor for per-editor toggles. Call when state that a
    // check depends on changes (toggles, editor focus, tree show/hide).
    void refreshMenuChecks() noexcept;
    // Refresh the recent-windows list in the Windows menu from the MRU order.
    void refreshWindowList() noexcept;
    // Focus the i-th most-recently-used editor window (Windows menu list).
    void focusRecentWindow(int index) noexcept;
    // Lowest window number in [1, 9] not currently used by an open editor, or
    // wnNoNumber if all nine are taken. Drives the built-in Alt-1..9 selection.
    short lowestFreeWindowNumber() noexcept;
    void onFilesChanged();
    // File-tree context-menu actions. Paths are absolute.
    void treeCreateFile(const std::string &dirPath);     // prompt + create + open
    void treeCreateFolder(const std::string &dirPath);   // prompt + mkdir
    void treeRenamePath(const std::string &path, bool isDir); // prompt + rename
    void treeStagePath(const std::string &path);         // git add
    void treeRevertPath(const std::string &path);        // confirm + git checkout HEAD
    // Reload an open editor's buffer from the file on disk (used after a revert,
    // so the editor doesn't keep -- and re-save -- the discarded changes). No-op
    // if 'path' isn't open in any editor.
    void reloadEditorFromDisk(const std::string &path) noexcept;
    // Reload every open editor that has no unsaved changes from disk (used after
    // a branch switch). Dirty buffers and files absent on the new branch are
    // left untouched, so no in-progress work is lost.
    void reloadCleanEditorsFromDisk() noexcept;

    // --- External-change detection -------------------------------------------
    // Capture 'w's file's current on-disk modification time + size as the
    // baseline for external-change detection. Called after opening, saving or
    // reloading the file; records no signature for a path-less scratch buffer.
    void rememberDiskSignature(EditorWindow &w) noexcept;
    // Reconcile an open editor with an external (non-git) change to 'path'
    // reported by the file watcher: silently reload a clean buffer, or ask
    // before discarding a buffer with unsaved edits. No-op when 'path' isn't
    // open, or its on-disk mod-time/size still match what we last recorded.
    void handleExternalFileChange(const std::string &path) noexcept;
    // >0 while an in-app git operation that rewrites the working tree is in
    // flight. Such an operation reloads its own editors from its completion
    // callback, so the watcher-driven external-change handler steps aside to
    // avoid a double reload or a redundant "changed on disk" prompt.
    int suppressExternalReload {0};

    // Menu-bar branch indicator (top-right view + on-demand popup).
    void refreshBranchView() noexcept;        // idle: update the indicator's text
    void showBranchMenu(TPoint where) noexcept; // build & run the branch popup
    void switchToBranch(const std::string &branch) noexcept; // confirm + checkout
    void gitNewBranch();       // prompt for a name, then create + switch to it
    void gitRefresh();
    void gitCommitDialog();
    void gitRemote(int which); // 0=fetch 1=pull 2=push
    void gitMerge();           // pick a branch + strategy, then merge
    void gitMergeAbort();      // git merge --abort
    void gitMergeContinue();   // commit the resolved merge
    void gitResolveFile(EditorWindow *w) noexcept; // save + git add a resolved file
    // Show/hide each open editor's conflict toolbar from git's unmerged set.
    void updateEditorConflictBars() noexcept;
    void configureLsp();
    // Detect the project's type (currently: Laravel, via an `artisan` file) and
    // enable the matching project-scoped language servers. Called on project
    // open; warns once per session if a detected server isn't installed.
    void autoConfigureProjectLsp() noexcept;
    bool warnedNoLaravelLsp {false};
    void editLspSettings();
    // Per-project debug-adapter settings dialog (edits .turbo/debug.json).
    void editDebugSettings();
    // Colour-scheme dialog. 'editThemeSettings' runs the dialog (which posts
    // cmApplyTheme on Apply/OK); 'applyActiveTheme' re-themes every open editor
    // from the now-current active schemes, repaints the chrome, and persists.
    void editThemeSettings();
    void applyActiveTheme() noexcept;
    // Switch the colour-depth mode ("auto"/"full"/"16"), persist it, and re-apply
    // the active schemes live. Some panels update fully only on the next launch.
    void setColorMode(const char *mode) noexcept;
    // Switch the file-tree glyph set ("auto"/"nerd"/"unicode"/"ascii"), persist
    // it, and repaint the tree. Fully live.
    void setTreeIcons(const char *mode) noexcept;
    void closeAll();
    TRect newEditorBounds() const;
    turbo::TScintilla &createScintilla() noexcept;
    void addEditor(turbo::TScintilla &, const char *path);

    // The currently focused editor window (front of the MRU list), or nullptr.
    EditorWindow *focusedEditor() noexcept;

    // --- Lua scripting ---------------------------------------------------
    // Construct the LuaManager, wire its host hooks to this app, and load init.lua
    // from the project and global Lua homes. Safe to call once the project root is
    // known (it is a no-op on a second call).
    void initLua() noexcept;
    // Fire an editor lifecycle event to the registered Lua handlers. Returns
    // false only when a "before*" handler cancelled the action.
    bool fireLuaEvent(const char *event) noexcept;
    bool fireLuaEvent(const char *event,
                      const std::vector<std::pair<std::string, std::string>> &params) noexcept;
    // Absolute paths of the runnable *.lua scripts in both Lua homes, project first:
    // <projectRoot>/turbo-scripts, then ~/.turbo/scripts. init.lua is excluded (it is
    // the hooks file, not a script to run). Used by the "Run Script" popup.
    std::vector<std::string> discoverLuaScripts() const noexcept;
    // Pop up the discovered-scripts menu and run the chosen one (cmLuaRunScript).
    void runLuaScriptPicker() noexcept;
    // Run the i-th script from discoverLuaScripts() (the cmLuaScriptBase + i
    // commands the command palette dispatches).
    void runDiscoveredLuaScript(int index) noexcept;
    // Prompt for a name and create+open a new script in the given scripts dir (a Lua
    // home's: <projectRoot>/turbo-scripts or ~/.turbo/scripts).
    void treeNewLuaScript(const std::string &dir) noexcept;
    // Prompt for a name and create+open a new script in the project Lua home
    // (<projectRoot>/turbo-scripts). Thin wrapper over treeNewLuaScript.
    void luaNewScript() noexcept;
    // Re-run init.lua from the project + global Lua homes (cmLuaReload).
    void reloadLuaConfig() noexcept;
    // Rescan both Lua homes (project / global) and push them into the tree as the
    // always-shown Lua "homes".
    void refreshLuaScriptsInTree() noexcept;
    // Prompt for a name and create+open a new skill (a <name>/SKILL.md folder,
    // pre-filled with a template) in the given skills dir.
    void treeNewSkill(const std::string &dir) noexcept;
    // Rescan the project + global agent skill dirs (.claude/skills, ~/.claude/
    // skills) and push them into the tree as the always-shown Skills "homes".
    void refreshSkillsInTree() noexcept;
    // Start watching ~/.claude/skills, if it exists. Retried after a skill is
    // created, since the directory may not have existed at startup.
    void startGlobalSkillsWatch() noexcept;
    void showEditorList(TEvent *ev);
    void toggleTreeView();
    void setTreeWidth(int w);     // resize the docked tree (from a left-border drag)

    // Build/Run output pane.
    TRect outputBounds() const;   // target bounds of the docked pane
    void toggleOutputView();      // show/hide it (resizes editors on the Y axis)
    void setOutputPaneHeight(int h); // resize the pane (from a top-border drag)
    void showOutput();            // ensure it is visible
    void runBuild();              // run the configured build (or prompt for one)
    void runTest();               // run the configured test command
    void runRun();                // build-if-needed, then run the run command
    void stopAll();               // stop the current build/run (tools keep going)
    void editBuildConfig();       // open the build-configuration dialog
    void editToolsConfig();       // open the tool-processes dialog
    // Stream 'command' (a shell command) into the output pane, with 'label'
    // shown as the echoed header line. 'onDone' (if set) runs on the main thread
    // with the exit code once the command finishes.
    void runInOutput(const std::string &label, const std::string &command,
                     std::function<void(int)> onDone = {});
    // Show a finished git command's output in the output pane (revealing the pane
    // if hidden): an echoed "$ <label>" header, the captured output, and -- on
    // failure -- the exit code. Wired to GitManager's output sink.
    void reportGitOutput(const std::string &label, int code,
                         const std::string &output);
    // Run the configured run command (tools are managed independently).
    void startRun();
    // --- Tool processes (Run menu toggles) -----------------------------------
    // Reconcile 'tools' with buildConfig.extra: preserve running processes whose
    // name+command are unchanged, stop+drop removed ones, add new slots, then
    // refresh the Run menu's tool toggles and their Output tabs.
    void applyToolConfig() noexcept;
    bool toolRunning(int i) const noexcept;
    void toggleTool(int i) noexcept;   // start it if stopped, else stop it
    void startTool(int i) noexcept;    // spawn + stream into its Output tab
    void stopTool(int i) noexcept;     // kill the process (keeps its Output tab)
    void stopAllTools() noexcept;      // stop every running tool (shutdown/close)
    void fillToolMenuLabels() noexcept; // write each tool item's name into the menu
    // True if Run should build first (per run mode / artifact staleness).
    bool needsBuildBeforeRun() const;
    // True if the configured artifact is missing or older than a project source.
    bool isArtifactStale() const;

    // --- Debugger (Debug Adapter Protocol) -----------------------------------
    // Construct the DapManager and wire its host callbacks to this app. Called
    // once during startup, after the output pane exists.
    void initDap() noexcept;
    // Start debugging the focused editor's file (resolves its language + adapter).
    void startDebug();
    // Terminate the current debug session (no-op when none is active).
    void stopDebug();
    // Route a debug-adapter 'output' event into the Debug Console output tab.
    void debugConsoleOutput(const std::string &category, const std::string &text) noexcept;
    // Clear the current-execution-line highlight from every open editor (on
    // continue / session end).
    void clearDebugCurrentLine() noexcept;

    // Terminal windows. newTerminal() opens one running the configured shell.
    // Each TerminalView registers itself so idle() can pump its PTY output.
    void newTerminal();
    void registerTerminal(TerminalView *t) noexcept;
    void unregisterTerminal(TerminalView *t) noexcept;

    // The dedicated coding-agent window. toggleAgent() opens it (running the
    // resolved agent command in the project dir) or focuses the existing one;
    // selectAgent() picks the per-project agent; restartAgent() reopens it.
    void toggleAgent();
    void selectAgent();
    void restartAgent();

    void handleFocus(EditorWindow &w) noexcept override;
    void handleTitleChange(EditorWindow &w) noexcept override;
    void removeEditor(EditorWindow &w) noexcept override;
    const char *getFileDialogDir() noexcept override;
    bool autoSaveOnFocusLoss() noexcept override;
    void editorTextChanged(EditorWindow &w) noexcept override;
    void editorWillSave(EditorWindow &w) noexcept override;
    void editorSaved(EditorWindow &w) noexcept override;
    void editorCharAdded(EditorWindow &w, int ch) noexcept override;
    void editorRequestCompletion(EditorWindow &w) noexcept override;
    void editorHoverStart(EditorWindow &w, long pos) noexcept override;
    void editorHoverEnd(EditorWindow &w) noexcept override;
    void editorToggleBreakpoint(EditorWindow &w, long line) noexcept override;
};

#endif
