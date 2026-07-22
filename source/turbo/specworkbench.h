#ifndef TURBO_SPECWORKBENCH_H
#define TURBO_SPECWORKBENCH_H

#define Uses_TView
#define Uses_TScrollBar
#include <tvision/tv.h>

#include <turbo/agentconversation.h>

#include <functional>
#include <memory>
#include <string>

#include "agentconversationview.h"
#include "specagentsession.h"

// The Spec Workbench's right-hand pane (M3 of specs/spec-agent-integration.md).
//
// FR1 asks for one window per spec: the document on the left, a conversational
// agent on the right. Rather than build a container and reparent the editor
// into it, the pane docks *into the spec's own EditorWindow* -- the same move
// setSpecSectionsMode already makes for the section strip, but horizontally.
// The window is therefore one window by construction, and the editor keeps
// every behaviour it already has (save, undo, LSP, the purple surface, the
// filewatcher reload, the section strip) because it is still the same window.
//
// Per-window state means several specs can be under discussion at once, which
// the single app-global agent terminal could never do.

struct EditorWindow;

// A one-column divider between the document and the conversation. Dragging it
// re-splits the window (FR1): the callback receives the window-local column the
// mouse is over on each move, and the owner clamps and re-lays-out.
struct SpecPaneDivider : public TView
{
    std::function<void(int localX)> onDrag;

    SpecPaneDivider(const TRect &bounds) noexcept;
    void draw() override;
    void handleEvent(TEvent &ev) override;
};

struct SpecAgentPane
{
    turbo::AgentConversation conv;
    std::unique_ptr<SpecAgentSession> session;
    AgentConversationView *view {nullptr};
    ComposerInputLine *composer {nullptr};
    TScrollBar *scrollBar {nullptr};
    SpecPaneDivider *divider {nullptr};
    // Columns given to the conversation, including the divider. Clamped on
    // layout so neither pane can be squeezed out of existence.
    int width {48};
    std::string command;

    ~SpecAgentPane();
};

// Smallest either pane may be squeezed to, in columns.
enum { specPaneMinEditor = 24, specPaneMinAgent = 24 };

#endif // TURBO_SPECWORKBENCH_H
