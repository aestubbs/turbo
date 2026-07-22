#ifndef TURBO_AGENTCONVERSATIONVIEW_H
#define TURBO_AGENTCONVERSATIONVIEW_H

#define Uses_TWindow
#define Uses_TListViewer
#define Uses_TInputLine
#define Uses_TScrollBar
#include <tvision/tv.h>

#include <turbo/agentconversation.h>

#include <functional>
#include <memory>
#include <string>

#include "specagentsession.h"

// The Workbench's right pane (M2 of specs/spec-agent-integration.md): a
// conversation with a coding agent, rendered from typed events rather than a
// terminal's character grid.
//
// The transcript itself is turbo::AgentConversation (pure model, in
// turbo-core); this file is the tvision face of it -- a scrolling view, a
// composer input line, and a window to host them until M3 folds the pair into
// the Spec Workbench container.

struct AgentConversationView;

// The message composer. TInputLine in this fork swallows Tab and Enter (see
// fieldinput.h), and the dialog-oriented FieldInputLine turns Enter into cmOK,
// which is wrong outside a dialog -- here Enter sends the turn.
struct ComposerInputLine : public TInputLine
{
    std::function<void(const std::string &)> onSubmit;
    std::function<void()> onFocusOut; // Tab: hand focus back to the transcript

    ComposerInputLine(const TRect &bounds, int maxLen) noexcept;
    void handleEvent(TEvent &ev) override;
};

// Scrolling transcript. Draws turbo::ConvLine rows straight from the model's
// layout, re-wrapping whenever the pane's width changes. Follows the tail
// unless the user has scrolled up, like the output pane.
struct AgentConversationView : public TListViewer
{
    turbo::AgentConversation *conv {nullptr}; // not owned
    bool followTail {true};
    // Fired when a row is activated (Enter or double-click) for an item that
    // names a place in the document -- the app moves the left pane's cursor
    // there (FR11). Kept a callback so the view stays decoupled from the
    // editor, like OutputView::onActivate.
    std::function<void(const turbo::ConvItem &item)> onActivate;
    // Tab out of the transcript (completes the pane focus cycle).
    std::function<void()> onFocusOut;

    AgentConversationView(const TRect &bounds, TScrollBar *vScrollBar,
                          turbo::AgentConversation *conv) noexcept;

    // Re-layout for the current width and refresh the scroll range. Call after
    // the model changes; scrolls to the tail when following.
    void refresh() noexcept;

    void draw() override;
    void changeBounds(const TRect &bounds) override; // re-wrap on resize
    void handleEvent(TEvent &ev) override;
    // Resolve the focused row to its item and fire onActivate.
    void activateFocused() noexcept;

private:
    int contentWidth() const noexcept { return size.x > 0 ? size.x : 1; }
};

// An ordinary window hosting a conversation with one agent session. M3 moves
// these parts into the Spec Workbench container; until then this is how the
// pane is exercised.
struct AgentChatWindow : public TWindow
{
    turbo::AgentConversation conv;
    std::unique_ptr<SpecAgentSession> session;
    AgentConversationView *view {nullptr};
    ComposerInputLine *composer {nullptr};
    TScrollBar *vScrollBar {nullptr};
    AgentChatWindow **backPtr {nullptr}; // nulled on close, like TerminalWindow
    std::string baseTitle {"Agent"};
    std::string titleBuf {"Agent"};

    // 'command' is the resolved agent command line, 'cwd' the project root.
    // The caller is responsible for confirming the launch with the user first
    // (the agent-launch confirmation lives in the app, as it does today).
    AgentChatWindow(const TRect &bounds, std::string command, std::string cwd,
                    std::string title, AgentChatWindow **backPtr = nullptr) noexcept;

    // Send a turn and echo it into the transcript.
    void submit(const std::string &text) noexcept;
    // Drain the session; call from the app's idle loop.
    void pump() noexcept;

    const char *getTitle(short) override;
    TColorAttr mapColor(uchar index) noexcept override;
    void setState(ushort aState, Boolean enable) override;
    void handleEvent(TEvent &ev) override;
    void shutDown() override;

private:
    void layoutPanes() noexcept;
    std::string command_, cwd_;
};

#endif // TURBO_AGENTCONVERSATIONVIEW_H
