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

#include <algorithm>
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
    // Own the wheel so scrolling acts on the pane the mouse is over, not on
    // whichever scrollbar a group broadcast happens to reach (the Spec
    // Workbench editor and this transcript share one window).
    eventMask |= evMouseWheel;
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
    if (ev.what == evMouseWheel)
    {
        // Scroll the viewport itself three rows. TListViewer scrolls by moving
        // 'focused', which only shifts topItem once the focus leaves the visible
        // window -- useless while tail-following (focus sits at the bottom). So
        // move topItem directly and keep focus inside it, mirroring the output
        // pane's feel.
        int step = (ev.mouse.wheel & mwUp) ? -3
                 : (ev.mouse.wheel & mwDown) ? 3 : 0;
        if (step && range > 0 && size.y > 0)
        {
            int maxTop = range > size.y ? range - size.y : 0;
            int newTop = std::min(std::max(topItem + step, 0), maxTop);
            if (newTop != topItem)
            {
                topItem = newTop;
                if (focused < topItem)
                    focused = topItem;
                else if (focused >= topItem + size.y)
                    focused = topItem + size.y - 1;
                if (vScrollBar)
                    vScrollBar->setValue(focused);
                drawView();
            }
        }
        if (conv)
        {
            int n = (int) conv->layout(contentWidth()).size();
            followTail = n == 0 || focused >= n - 1;
        }
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
