#define Uses_TWindow
#define Uses_TFrame
#define Uses_TView
#define Uses_TListViewer
#define Uses_TInputLine
#define Uses_TScrollBar
#define Uses_TDrawBuffer
#define Uses_TEvent
#define Uses_TKeys
#define Uses_TProgram
#define Uses_TEventQueue
#include <tvision/tv.h>

#include "agentconversationview.h"
#include "cmds.h"

#include <turbo/basicwindow.h> // shared window-chrome scheme

#include <cstdint>
#include <cstring>

namespace {

// The window background: the unified blue when active, the dimmer passive
// shade when not -- matching the editors, tree and output pane.
TColorDesired paneBg(TView *owner, bool active) noexcept
{
    if (!owner)
        return TColorRGB(0x10182E);
    return getBack(owner->mapColor((active ? turbo::wndFrameActive
                                           : turbo::wndFramePassive) + 1));
}

TColorRGB lighten(TColorDesired c, int pct) noexcept
{
    uint32_t v = (uint32_t) c.asRGB();
    int r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
    auto m = [&] (int x) { return (uint8_t) (x + (255 - x) * pct / 100); };
    return TColorRGB(m(r), m(g), m(b));
}

constexpr int composerMax = 1024;

} // namespace

// ---------------------------------------------------------------------------
// ComposerInputLine

ComposerInputLine::ComposerInputLine(const TRect &bounds, int maxLen) noexcept :
    TInputLine(bounds, maxLen)
{
}

void ComposerInputLine::handleEvent(TEvent &ev)
{
    if (ev.what == evKeyDown)
    {
        ushort key = ev.keyDown.keyCode;
        if (key == kbEnter)
        {
            // Enter sends the turn. (FieldInputLine maps it to cmOK, which is
            // right in a dialog and wrong here.)
            std::string text = data ? data : "";
            if (!text.empty() && onSubmit)
            {
                onSubmit(text);
                if (data)
                    data[0] = '\0';
                selectAll(True);
                drawView();
            }
            clearEvent(ev);
            return;
        }
        if (key == kbTab || key == kbShiftTab)
        {
            if (onFocusOut)
                onFocusOut();
            else if (owner)
                owner->selectNext(Boolean(key == kbShiftTab));
            clearEvent(ev);
            return;
        }
    }
    TInputLine::handleEvent(ev);
}

// ---------------------------------------------------------------------------
// AgentConversationView

AgentConversationView::AgentConversationView(const TRect &bounds,
                                             TScrollBar *vScrollBar,
                                             turbo::AgentConversation *aConv) noexcept :
    TListViewer(bounds, 1, nullptr, vScrollBar),
    conv(aConv)
{
    options |= ofFirstClick | ofSelectable;
    growMode = gfGrowHiX | gfGrowHiY;
    setRange(0);
}

void AgentConversationView::refresh() noexcept
{
    if (!conv)
        return;
    int n = (int) conv->layout(contentWidth()).size();
    setRange(n);
    if (followTail && n > 0)
        focusItem(n - 1);
    drawView();
}

void AgentConversationView::changeBounds(const TRect &bounds)
{
    TListViewer::changeBounds(bounds);
    // The pane's width changed, so the wrap did too.
    refresh();
}

void AgentConversationView::draw()
{
    bool winActive = owner && (owner->state & sfActive);
    TColorDesired bg = paneBg(owner, winActive);

    // One colour per item kind. Colour and indentation carry the structure --
    // glyph coverage cannot be detected at runtime, so no exotic markers.
    TColorAttr cAssistant {TColorRGB(0xCBD6F2), bg};
    TColorAttr cUser      {TColorRGB(0xE8C07D), bg}; // gold, like the hotkeys
    TColorAttr cTool      {TColorRGB(0x8FD0C4), bg};
    TColorAttr cResult    {TColorRGB(0x8892B0), bg};
    TColorAttr cError     {TColorRGB(0xFF8B8B), bg};
    TColorAttr cNotice    {TColorRGB(0xAEC9FF), bg};
    TColorAttr cPending   {TColorRGB(0x9A8763), bg}; // a turn still in flight
    TColorRGB focusBg = lighten(bg, 26);

    const std::vector<turbo::ConvLine> *lines = nullptr;
    if (conv)
        lines = &conv->layout(contentWidth());
    int n = lines ? (int) lines->size() : 0;
    bool focusedView = winActive && (state & sfSelected) != 0;

    for (int y = 0; y < size.y; ++y)
    {
        int idx = topItem + y;
        TDrawBuffer b;
        b.moveChar(0, ' ', cAssistant, size.x);
        if (lines && idx >= 0 && idx < n)
        {
            const turbo::ConvLine &ln = (*lines)[idx];
            TColorAttr c = cAssistant;
            switch (ln.kind)
            {
                case turbo::ConvKind::UserTurn:
                    c = ln.pending ? cPending : cUser;
                    break;
                case turbo::ConvKind::ToolUse:    c = cTool;   break;
                case turbo::ConvKind::ToolResult: c = ln.isError ? cError : cResult; break;
                case turbo::ConvKind::Error:      c = cError;  break;
                case turbo::ConvKind::Notice:     c = cNotice; break;
                case turbo::ConvKind::Assistant:
                default:                          c = cAssistant; break;
            }
            if (focusedView && idx == focused)
                ::setBack(c, focusBg);
            b.moveChar(0, ' ', c, size.x);
            // moveStr, not moveChar: the transcript is UTF-8 and moveChar
            // drops high bytes.
            if (!ln.text.empty())
                b.moveStr(0, ln.text.c_str(), c);
        }
        writeLine(0, y, size.x, 1, b);
    }
}

void AgentConversationView::activateFocused() noexcept
{
    if (!conv || !onActivate)
        return;
    const auto &lines = conv->layout(contentWidth());
    if (focused < 0 || focused >= (int) lines.size())
        return;
    size_t idx = lines[focused].item;
    if (idx < conv->items().size())
        onActivate(conv->items()[idx]);
}

void AgentConversationView::handleEvent(TEvent &ev)
{
    // Enter / double-click on a tool call jumps the document to what it
    // touched. Handled before TListViewer so Enter is not swallowed as a
    // selection event.
    if (ev.what == evKeyDown && ev.keyDown.keyCode == kbEnter)
    {
        activateFocused();
        clearEvent(ev);
        return;
    }
    if (ev.what == evKeyDown &&
        (ev.keyDown.keyCode == kbTab || ev.keyDown.keyCode == kbShiftTab))
    {
        if (onFocusOut)
            onFocusOut();
        else if (owner)
            owner->selectNext(Boolean(ev.keyDown.keyCode == kbShiftTab));
        clearEvent(ev);
        return;
    }
    if (ev.what == evMouseDown && (ev.mouse.eventFlags & meDoubleClick))
    {
        TPoint m = makeLocal(ev.mouse.where);
        focused = (short) (topItem + m.y);
        activateFocused();
        drawView();
        clearEvent(ev);
        return;
    }
    TListViewer::handleEvent(ev);
    // Any navigation that leaves the last row parks the tail-follow; returning
    // to the bottom resumes it. Same behaviour as the output pane.
    if (conv)
    {
        int n = (int) conv->layout(contentWidth()).size();
        followTail = n == 0 || focused >= n - 1;
    }
}

// ---------------------------------------------------------------------------
// AgentChatWindow

AgentChatWindow::AgentChatWindow(const TRect &bounds, std::string command,
                                 std::string cwd, std::string title,
                                 AgentChatWindow **aBackPtr) noexcept :
    TWindowInit(&TWindow::initFrame),
    TWindow(bounds, title.c_str(), wnNoNumber),
    backPtr(aBackPtr),
    baseTitle(title),
    titleBuf(std::move(title)),
    command_(std::move(command)),
    cwd_(std::move(cwd))
{
    options |= ofTileable;
    state &= ~sfShadow;

    TRect r = getExtent().grow(-1, -1);
    int composerY = r.b.y - 1;

    vScrollBar = new TScrollBar(TRect(size.x - 1, 1, size.x, composerY));
    vScrollBar->growMode = gfGrowLoX | gfGrowHiX | gfGrowHiY;
    insert(vScrollBar);

    view = new AgentConversationView(TRect(r.a.x, r.a.y, r.b.x, composerY),
                                     vScrollBar, &conv);
    insert(view);

    composer = new ComposerInputLine(TRect(r.a.x, composerY, r.b.x, composerY + 1),
                                     composerMax);
    composer->growMode = gfGrowLoY | gfGrowHiX | gfGrowHiY;
    composer->onSubmit = [this] (const std::string &text) { submit(text); };
    composer->onFocusOut = [this] { if (view) view->select(); };
    insert(composer);

    session.reset(new SpecAgentSession());
    session->onWake = [] { TEventQueue::wakeUp(); };
    session->onEvent = [this] (const turbo::SpecAgentEvent &e) {
        if (conv.addEvent(e) && view)
            view->refresh();
        // The session id only becomes known once the agent reports it; show it
        // so a resume is possible after the window closes.
        if (e.kind == turbo::SpecAgentEventKind::SessionStart && frame)
            frame->drawView();
    };

    if (!session->start(command_, cwd_))
    {
        conv.addNotice("Could not start the agent: " + command_);
        if (view)
            view->refresh();
    }
    else
        conv.addNotice("Agent started. Type a message and press Enter.");
    if (view)
        view->refresh();
    composer->select();
}

void AgentChatWindow::submit(const std::string &text) noexcept
{
    if (!session)
        return;
    conv.addUserTurn(text);
    if (!session->send(text))
        conv.addNotice("The agent is not running; the message was not sent.");
    if (view)
    {
        view->followTail = true;
        view->refresh();
    }
}

void AgentChatWindow::pump() noexcept
{
    if (session)
        session->pump();
}

const char *AgentChatWindow::getTitle(short)
{
    titleBuf = baseTitle;
    if (session && session->busy())
        titleBuf += " (working)";
    else if (session && !session->running())
        titleBuf += " (stopped)";
    return titleBuf.c_str();
}

TColorAttr AgentChatWindow::mapColor(uchar index) noexcept
{
    // Resolve chrome through the shared window scheme so the frame and
    // scrollbar match the editors, tree and output pane.
    if (index > 0 && index - 1 < turbo::WindowPaletteItemCount)
        return turbo::windowSchemeActive[index - 1];
    return errorAttr;
}

void AgentChatWindow::setState(ushort aState, Boolean enable)
{
    TWindow::setState(aState, enable);
    if (aState == sfActive)
        redraw(); // the transcript's background tracks the active state
}

void AgentChatWindow::layoutPanes() noexcept
{
    // Kept for M3, when the panes move into the Workbench container and the
    // splitter drives this.
}

void AgentChatWindow::handleEvent(TEvent &ev)
{
    if (ev.what == evKeyDown && ev.keyDown.keyCode == kbTab && view &&
        (view->state & sfSelected))
    {
        if (composer)
            composer->select();
        clearEvent(ev);
        return;
    }
    TWindow::handleEvent(ev);
}

void AgentChatWindow::shutDown()
{
    if (session)
        session->stop();
    if (backPtr)
    {
        *backPtr = nullptr;
        backPtr = nullptr;
    }
    view = nullptr;
    composer = nullptr;
    vScrollBar = nullptr;
    TWindow::shutDown();
}
