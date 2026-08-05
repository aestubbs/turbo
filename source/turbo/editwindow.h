#ifndef TURBO_EDITWINDOW_H
#define TURBO_EDITWINDOW_H

#define Uses_TWindow
#define Uses_TPalette
#include <tvision/tv.h>

#include <turbo/fileeditor.h>
#include <turbo/basicwindow.h>
#include <turbo/basicframe.h>
#include <turbo/agentconversation.h> // turbo::ConvItem (Workbench pane)
#include "apputils.h"
#include "editor.h"
#include "search.h"

#include <cstdint>
#include <filesystem>

struct FileNumberState
{
    active_counter *counter;
    uint number;

    FileNumberState(active_counter &aCounter) noexcept;
    ~FileNumberState();
    FileNumberState &operator=(FileNumberState &&other) noexcept;
};

struct TitleState
{
    active_counter *counter;
    uint number;
    bool inSavePoint;
    short windowNumber;

    bool operator!=(const TitleState &other) const noexcept
    {
        return !( counter == other.counter && number == other.number &&
                  inSavePoint == other.inSavePoint &&
                  windowNumber == other.windowNumber );
    }
};

struct SpecAgentPane;

struct EditorWindow;

// Editor window frame with a "reveal in tree" [>] button at the top-right,
// just left of the zoom icon. Clicking it highlights the file in the tree view.
struct EditorFrame : public turbo::BasicEditorFrame
{
    EditorFrame(const TRect &bounds) noexcept;
    void draw() override;
    void handleEvent(TEvent &ev) override;
};

// A one-row toolbar docked at the top of an editor window (just under the title
// bar), shown only while the file is in a git merge-conflict state. It is shaded
// to match the active window frame and hosts [Button]-style clickable controls
// for navigating and resolving the conflict markers git wrote into the file.
struct EditorConflictBar : public TView
{
    EditorWindow *win {nullptr};
    enum Btn { bPrev, bNext, bOurs, bTheirs, bBoth, bResolve, bAbort, bCount };
    int hx0[bCount] {};   // per-button hit ranges, recomputed each draw
    int hx1[bCount] {};

    EditorConflictBar(const TRect &bounds, EditorWindow *win) noexcept;

    void draw() override;
    void handleEvent(TEvent &ev) override;
};

// A one-row strip docked at the top of a spec editor (the Workbench's FR6
// status strip): section count plus which canonical sections are still
// empty, recomputed from the document like the conflict bar. Shaded to
// match the frame.
struct SpecSectionBar : public TView
{
    EditorWindow *win {nullptr};

    SpecSectionBar(const TRect &bounds, EditorWindow *win) noexcept;

    void draw() override;
};

struct EditorWindowParent
{
    virtual void handleFocus(EditorWindow &w) noexcept = 0;
    virtual void handleTitleChange(EditorWindow &w) noexcept = 0;
    virtual void removeEditor(EditorWindow &w) noexcept = 0;
    virtual const char *getFileDialogDir() noexcept = 0;
    virtual bool autoSaveOnFocusLoss() noexcept = 0;
    // The document's text changed / was saved (for language-server sync).
    virtual void editorTextChanged(EditorWindow &w) noexcept {}
    // About to write the document to disk (explicit save or auto-save).
    virtual void editorWillSave(EditorWindow &w) noexcept {}
    virtual void editorSaved(EditorWindow &w) noexcept {}
    // A character was typed (for completion trigger characters).
    virtual void editorCharAdded(EditorWindow &w, int ch) noexcept {}
    // Explicit completion request (Edit > Complete).
    virtual void editorRequestCompletion(EditorWindow &w) noexcept {}
    // Mouse dwell start/end over a document position (for hover).
    virtual void editorHoverStart(EditorWindow &w, long pos) noexcept {}
    virtual void editorHoverEnd(EditorWindow &w) noexcept {}
    // Toggle a debugger breakpoint on document line 'line' (0-based). The app
    // owns the breakpoint model (DapManager) and updates the gutter marker.
    virtual void editorToggleBreakpoint(EditorWindow &w, long line) noexcept {}
    // Record the agent session id a Workbench pane captured, keyed by the spec
    // path, so reopening the Workbench can resume the conversation with
    // --resume rather than starting over (spec-agent-integration FR7).
    virtual void rememberSpecSession(const std::string &specPath,
                                     const std::string &sessionId) noexcept {}
    // The remembered session id for a spec, or empty if none.
    virtual std::string recallSpecSession(const std::string &specPath) noexcept
        { return {}; }
};

struct EditorWindow : public turbo::BasicEditorWindow
{
    using super = turbo::BasicEditorWindow;

    list_head<EditorWindow> listHead;
    FileNumberState fileNumber;
    EditorWindowParent &parent;
    TitleState lastTitleState {};
    std::string title;
    TCommandSet enabledCmds, disabledCmds;

    TView *bottomView {nullptr};
    EditorConflictBar *conflictBar {nullptr};
    SpecSectionBar *specBar {nullptr};
    // Non-null while the Workbench pane is docked (see specworkbench.h).
    SpecAgentPane *agentPane {nullptr};
    SearchState searchState;

    // External-change detection: the modification time and size of filePath() on
    // disk the last time we read or wrote it. TurboApp::onFilesChanged() compares
    // the current on-disk values against these to tell a change made by another
    // process apart from our own save. 'diskSigValid' stays false until a
    // signature is first captured (e.g. an unsaved scratch buffer has no file).
    std::filesystem::file_time_type diskModTime {};
    std::uintmax_t diskSize {0};
    bool diskSigValid {false};

    // The file lives under <project-root>/specs/: render the text on the deep
    // purple spec surface (magenta in classic 16-colour mode). Set once at
    // creation by TurboApp::addEditor(); frames keep their normal treatment.
    bool isSpec {false};

    EditorWindow( const TRect &bounds, TurboEditor &aEditor, active_counter &fileCounter,
                  turbo::SearchSettings &searchSettings, EditorWindowParent &aParent ) noexcept;

    static TFrame *initFrame(TRect bounds);

    void shutDown() override;
    void handleEvent(TEvent &ev) override;
    void setState(ushort aState, Boolean enable) override;
    // Re-applies the Workbench split; a no-op without an agent pane.
    void changeBounds(const TRect &bounds) override;
    Boolean valid(ushort command) override;
    const char *getTitle(short = 0) override;
    void sizeLimits(TPoint &min, TPoint &max) override;
    void updateCommands() noexcept;
    void handleNotification(const SCNotification &scn, turbo::Editor &) override;

    // Re-theme the editor so its background follows the window's active state
    // (the unified blue when active, a dimmer shade when not), matching the
    // other windows. Called on activation changes.
    void applyActiveStateTheme() noexcept;

    // Show/hide the merge-conflict toolbar at the top of the window. Idempotent;
    // driven from git status (the file's Conflicted/unmerged state).
    void setConflictMode(bool on) noexcept;

    // Show/hide the spec section-status strip (Workbench FR6). Idempotent.
    void setSpecSectionsMode(bool on) noexcept;

    // The Spec Workbench's agent pane (specs/spec-agent-integration.md FR1):
    // a live agent conversation docked into the right of this window, so the
    // spec and its agent are one window rather than two placed side by side.
    // Per-window, so several specs can be under discussion at once.
    void setAgentPaneMode(bool on, const std::string &command = {},
                          const std::string &cwd = {},
                          const std::string &resumeSessionId = {}) noexcept;
    bool hasAgentPane() const noexcept { return agentPane != nullptr; }
    // The window column of the Workbench divider, or -1 when no pane is docked.
    // EditorFrame uses it to draw the ┬/┴ where the divider meets the frame.
    int agentDividerColumn() const noexcept;
    // Drain the pane's agent session; called from the app's idle loop.
    void pumpAgentPane() noexcept;
    // Move focus to the conversation ('toAgent') or back to the document.
    // Scintilla owns Tab for indentation, so crossing out of the document
    // needs its own command rather than a key the editor would swallow.
    void focusAgentPane(bool toAgent) noexcept;
    bool agentPaneHasFocus() const noexcept;
    // FR11: move the document's cursor to what a tool call touched.
    // jumpToToolTarget() uses the most recent such call (the keyboard
    // path); jumpToToolItem() is the click path. Both return false when
    // the call names no part of the document.
    bool jumpToToolItem(const turbo::ConvItem &item) noexcept;
    bool jumpToToolTarget() noexcept;
    // Send one turn to the pane's agent (used to deliver the opening brief).
    void sendToAgentPane(const std::string &text) noexcept;
    // Re-apply the document/conversation split after a resize.
    void layoutAgentPane() noexcept;

    void closeBottomView();
    void setBottomView(TView *view);
    template <class T, class ...Args>
    void openBottomView(Args&& ...args);

    enum TitleFormatFlags
    {
        tfNoSavePoint = 0x0001,
    };

    const char *formatTitle(ushort flags = 0) noexcept;

    auto &getEditor() { return (TurboEditor &) super::editor; }
    auto &filePath() { return getEditor().filePath; }

};

inline FileNumberState::FileNumberState(active_counter &aCounter) noexcept :
    counter(&aCounter),
    number(++aCounter)
{
}

inline FileNumberState::~FileNumberState()
{
    --*counter;
}

inline FileNumberState &FileNumberState::operator=(FileNumberState &&other) noexcept
{
    std::swap(counter, other.counter);
    std::swap(number, other.number);
    return *this;
}

#endif
