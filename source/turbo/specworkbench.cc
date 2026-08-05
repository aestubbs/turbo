#define Uses_TWindow
#define Uses_TView
#define Uses_TScrollBar
#define Uses_TDrawBuffer
#define Uses_TEvent
#define Uses_TKeys
#define Uses_TProgram
#define Uses_TEventQueue
#include <tvision/tv.h>

#include "specworkbench.h"
#include "editwindow.h"
#include "speccolors.h"

#include <turbo/util.h> // forEachNotNull
#include <turbo/scintilla.h> // getRangePointer, SCI_* constants

#include <algorithm>

// ---------------------------------------------------------------------------
// SpecPaneDivider

SpecPaneDivider::SpecPaneDivider(const TRect &bounds) noexcept : TView(bounds)
{
    growMode = gfGrowLoX | gfGrowHiX | gfGrowHiY;
    // The whole column is a grab handle for the drag.
    eventMask |= evMouseAuto;
}

void SpecPaneDivider::handleEvent(TEvent &ev)
{
    TView::handleEvent(ev);
    if (ev.what == evMouseDown)
    {
        // Drag to move the split (FR1). Report the window-local column under
        // the mouse on each move; the owner clamps it and re-lays-out. The
        // standard mouseEvent() loop keeps the grab until the button is up.
        do {
            if (owner && onDrag)
            {
                TPoint p = owner->makeLocal(ev.mouse.where);
                onDrag(p.x);
            }
        } while (mouseEvent(ev, evMouseMove | evMouseAuto));
        clearEvent(ev);
    }
}

void SpecPaneDivider::draw()
{
    bool active = owner && (owner->state & sfActive);
    TColorAttr c {turbo::specSurfaceBg(!active), turbo::specSurfaceBg(active)};
    TDrawBuffer b;
    for (int y = 0; y < size.y; ++y)
    {
        b.moveChar(0, ' ', c, size.x);
        // Box-drawing is used unconditionally elsewhere in the chrome, so the
        // glyph is safe; moveStr because moveChar drops high bytes.
        b.moveStr(0, "\xE2\x94\x82", c); // U+2502
        writeLine(0, y, size.x, 1, b);
    }
}

SpecAgentPane::~SpecAgentPane()
{
    if (session)
        session->stop();
}

// ---------------------------------------------------------------------------
// Editor geometry. The existing growEditor/shiftEditorTop helpers move the
// editor's bottom and top edges for the conflict bar and section strip; this
// is the same idea on the horizontal axis.

static void setEditorRightEdge(turbo::Editor &editor, int rightX)
{
    if (editor.view)
    {
        TRect r = editor.view->getBounds();
        r.b.x = std::max(r.a.x + 1, rightX - 1); // leave the scrollbar column
        editor.view->setBounds(r);
    }
    if (editor.hScrollBar)
    {
        TRect r = editor.hScrollBar->getBounds();
        r.b.x = std::max(r.a.x + 1, rightX - 2);
        editor.hScrollBar->setBounds(r);
    }
    if (editor.vScrollBar)
    {
        TRect r = editor.vScrollBar->getBounds();
        int w = std::max(1, r.b.x - r.a.x);
        r.b.x = rightX;
        r.a.x = rightX - w;
        editor.vScrollBar->setBounds(r);
    }
}

// ---------------------------------------------------------------------------
// EditorWindow: the Workbench pane

void EditorWindow::layoutAgentPane() noexcept
{
    if (!agentPane)
        return;
    int left = 1, right = size.x - 1;
    int top = 1 + (conflictBar ? 1 : 0) + (specBar ? 1 : 0);
    int contentBottom = size.y - 1;
    if (right - left < specPaneMinEditor + specPaneMinAgent ||
        contentBottom - top < 2)
        return; // too small to split; leave the document whole

    int maxAgent = right - left - specPaneMinEditor;
    int agentW = std::min(std::max(agentPane->width, (int) specPaneMinAgent), maxAgent);
    agentPane->width = agentW;
    int split = right - agentW;          // the divider's column
    int composerY = contentBottom - 1;   // one row for the composer

    setEditorRightEdge(editor, split);
    if (specBar)
    {
        // The strip belongs to the document, so it stops at the divider.
        TRect r = specBar->getBounds();
        r.b.x = split;
        specBar->setBounds(r);
    }
    if (agentPane->divider)
        // Run the divider the full inner height (just under the top border to
        // just above the bottom), not only the content rows: column 'split' is
        // free at the strip row (specBar stops at 'split' below), so the line is
        // continuous and EditorFrame draws the ┬/┴ where it meets the frame.
        agentPane->divider->setBounds(TRect(split, 1, split + 1, contentBottom));
    if (agentPane->view)
        agentPane->view->setBounds(TRect(split + 1, top, right, composerY));
    if (agentPane->scrollBar)
        agentPane->scrollBar->setBounds(TRect(right, top, right + 1, composerY));
    if (agentPane->composer)
        agentPane->composer->setBounds(TRect(split + 1, composerY, right, composerY + 1));
}

void EditorWindow::changeBounds(const TRect &bounds)
{
    super::changeBounds(bounds);
    layoutAgentPane(); // absolute re-split; immune to the subviews' growMode
}

int EditorWindow::agentDividerColumn() const noexcept
{
    return agentPane && agentPane->divider ? agentPane->divider->origin.x : -1;
}

void EditorWindow::setAgentPaneMode(bool on, const std::string &command,
                                    const std::string &cwd,
                                    const std::string &resumeSessionId) noexcept
{
    if (on == (agentPane != nullptr))
        return; // idempotent, like setSpecSectionsMode

    if (!on)
    {
        // Give the columns back to the document.
        SpecAgentPane *p = agentPane;
        agentPane = nullptr;
        turbo::forEachNotNull([&] (TView &v) { TObject::destroy(&v); },
                              (TView *) p->view, (TView *) p->composer,
                              (TView *) p->scrollBar, (TView *) p->divider);
        delete p;
        setEditorRightEdge(editor, size.x - 1);
        if (specBar)
        {
            TRect r = specBar->getBounds();
            r.b.x = size.x - 1;
            specBar->setBounds(r);
        }
        editor.redraw();
        redraw();
        return;
    }

    agentPane = new SpecAgentPane();
    agentPane->command = command;

    // Bounds are provisional; layoutAgentPane() sets the real geometry once
    // every subview exists.
    TRect r = getExtent();
    r.grow(-1, -1);
    agentPane->divider = new SpecPaneDivider(TRect(r.a.x, r.a.y, r.a.x + 1, r.b.y));
    agentPane->divider->onDrag = [this] (int localX) {
        if (!agentPane)
            return;
        // The agent pane takes the columns to the right of the divider;
        // layoutAgentPane() clamps the width so neither pane is squeezed out.
        agentPane->width = (size.x - 1) - localX;
        layoutAgentPane();
        editor.redraw();
        redraw();
    };
    insert(agentPane->divider);
    agentPane->scrollBar = new TScrollBar(TRect(r.b.x, r.a.y, r.b.x + 1, r.b.y - 1));
    agentPane->scrollBar->growMode = gfGrowLoX | gfGrowHiX | gfGrowHiY;
    insert(agentPane->scrollBar);
    agentPane->view = new AgentConversationView(
        TRect(r.a.x, r.a.y, r.a.x + 1, r.b.y - 1), agentPane->scrollBar,
        &agentPane->conv);
    insert(agentPane->view);
    agentPane->composer = new ComposerInputLine(
        TRect(r.a.x, r.b.y - 1, r.a.x + 1, r.b.y), 1024);
    agentPane->composer->growMode = gfGrowLoY | gfGrowHiX | gfGrowHiY;
    agentPane->composer->onSubmit = [this] (const std::string &text) {
        sendToAgentPane(text);
    };
    agentPane->composer->onFocusOut = [this] {
        // Tab from the composer reaches the transcript, so its rows can be
        // navigated and activated; without this the tool-call jump (FR11)
        // would be mouse-only.
        if (agentPane && agentPane->view)
            agentPane->view->select();
    };
    insert(agentPane->composer);

    // FR11: activating a tool call moves the document's cursor to whatever
    // the agent touched, so the transcript is a way *into* the spec rather
    // than a log beside it. Double-click goes through here; the keyboard
    // path is jumpToToolTarget() below.
    agentPane->view->onActivate = [this] (const turbo::ConvItem &item) {
        jumpToToolItem(item);
    };

    agentPane->view->onFocusOut = [this] {
        if (editor.view)
            editor.view->select();
    };

    agentPane->session.reset(new SpecAgentSession());
    agentPane->session->onWake = [] { TEventQueue::wakeUp(); };
    agentPane->session->onEvent = [this] (const turbo::SpecAgentEvent &e) {
        if (!agentPane)
            return;
        // Remember the agent's own session id the moment it arrives, keyed by
        // this spec, so reopening the Workbench can resume it (FR7).
        if (e.kind == turbo::SpecAgentEventKind::SessionStart &&
            !e.sessionId.empty())
            parent.rememberSpecSession(std::string(filePath()), e.sessionId);
        if (agentPane->conv.addEvent(e) && agentPane->view)
            agentPane->view->refresh();
    };

    layoutAgentPane();

    if (!agentPane->session->start(command, cwd, resumeSessionId))
        agentPane->conv.addNotice("Could not start the agent: " + command);
    else if (!resumeSessionId.empty())
        agentPane->conv.addNotice("Resuming the previous conversation. "
                                  "Alt-Right/Alt-Left move between the document "
                                  "and the conversation.");
    else
        agentPane->conv.addNotice("Agent ready. Alt-Right/Alt-Left move between "
                                  "the document and the conversation.");
    if (agentPane->view)
        agentPane->view->refresh();
    // The document is primary (UX), and inserting selectable views makes the
    // group re-pick its current view -- so say which pane owns focus rather
    // than depending on TGroup::resetCurrent's choice.
    if (editor.view)
        editor.view->select();
    editor.redraw();
    redraw();
}

void EditorWindow::sendToAgentPane(const std::string &text) noexcept
{
    if (!agentPane || text.empty())
        return;
    agentPane->conv.addUserTurn(text);
    if (!agentPane->session || !agentPane->session->send(text))
        agentPane->conv.addNotice("The agent is not running; nothing was sent.");
    if (agentPane->view)
    {
        agentPane->view->followTail = true;
        agentPane->view->refresh();
    }
}

void EditorWindow::pumpAgentPane() noexcept
{
    if (agentPane && agentPane->session)
        agentPane->session->pump();
}

void EditorWindow::focusAgentPane(bool toAgent) noexcept
{
    if (!agentPane)
        return;
    // Three stops, cycled by Alt-Right (forward) and Alt-Left (back):
    //   document -> composer -> transcript -> document
    // The transcript needs its own stop so tool-call rows can be navigated
    // and activated (FR11); reaching it via Tab is not available, because
    // TWindow::handleEvent claims Tab for focusNext before a subview sees it.
    bool inComposer = agentPane->composer &&
                      (agentPane->composer->state & sfSelected) != 0;
    bool inTranscript = agentPane->view &&
                        (agentPane->view->state & sfSelected) != 0;
    TView *next = nullptr;
    if (toAgent)
        next = inComposer   ? (TView *) agentPane->view
             : inTranscript ? (TView *) editor.view
                            : (TView *) agentPane->composer;
    else
        next = inTranscript ? (TView *) agentPane->composer
             : inComposer   ? (TView *) editor.view
                            : (TView *) agentPane->view;
    if (next)
        next->select();
}

bool EditorWindow::agentPaneHasFocus() const noexcept
{
    return agentPane &&
           ((agentPane->composer && (agentPane->composer->state & sfSelected)) ||
            (agentPane->view && (agentPane->view->state & sfSelected)));
}

bool EditorWindow::jumpToToolItem(const turbo::ConvItem &item) noexcept
{
    if (item.kind != turbo::ConvKind::ToolUse)
        return false;
    // Read the live buffer, not the file on disk: the user may have unsaved
    // edits, and the jump must land where the text actually is.
    long len = (long) editor.callScintilla(SCI_GETLENGTH, 0U, 0U);
    TStringView sv = turbo::getRangePointer(editor.scintilla, 0, len);
    long line = turbo::convToolTargetLine(
        item.toolInput, turbo::specSections(std::string(sv.data(), sv.size())));
    if (line < 0)
        return false;
    editor.callScintilla(SCI_GOTOLINE, (uptr_t) line, 0U);
    // GOTOLINE moves the caret but does not scroll; SCROLLCARET alone scrolls
    // minimally, which lands a section heading on the bottom edge with its
    // content still below the fold. Put the target near the top instead, so
    // the jump shows the section *and* what the agent wrote in it.
    editor.callScintilla(SCI_SCROLLCARET, 0U, 0U);
    editor.callScintilla(SCI_SETFIRSTVISIBLELINE,
                         (uptr_t) (line > 0 ? line - 1 : 0), 0U);
    editor.redraw();
    if (editor.view)
        editor.view->select(); // land in the document, ready to read/edit
    return true;
}

bool EditorWindow::jumpToToolTarget() noexcept
{
    if (!agentPane)
        return false;
    // The most recent tool call that names somewhere in the document. Walking
    // backwards means the keyboard path always follows what the agent just
    // did, which is what a reader wants while a turn is in flight.
    const auto &items = agentPane->conv.items();
    for (size_t i = items.size(); i-- > 0;)
        if (items[i].kind == turbo::ConvKind::ToolUse &&
            jumpToToolItem(items[i]))
            return true;
    return false;
}
