#define Uses_MsgBox
#define Uses_TDrawBuffer
#define Uses_TEvent
#define Uses_TKeys
#define Uses_TProgram
#define Uses_TDeskTop
#include <tvision/tv.h>

#include "specmanager.h"

#include <turbo/basicwindow.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

using namespace turbo;

namespace {

// Column widths (plus one space between columns); Title takes the rest.
constexpr int kStatusW = 13, kDomainW = 12, kUpdatedW = 10, kPlanW = 5;

std::string today() noexcept
{
    char date[16];
    std::time_t now = std::time(nullptr);
    std::strftime(date, sizeof date, "%Y-%m-%d", std::localtime(&now));
    return date;
}

// Background/foreground for the manager rows, tracking the window's active
// state like the tree and output panes. Classic 16-colour mode gets BIOS
// pairs instead of quantised RGB.
struct RowColors
{
    TColorAttr normal, focusedRow, headerFg, blocked, pass;
};

RowColors rowColors(TGroup *owner) noexcept
{
    bool active = owner && (owner->state & sfActive);
    TColorDesired bg =
        ::getBack(windowSchemeActive[active ? wndFrameActive : wndFramePassive]);
    RowColors c;
    if (bg.isBIOS())
    {
        TColorDesired biosBg = bg;
        c.normal     = TColorAttr {TColorDesired(uchar(0x7)), biosBg};
        c.focusedRow = TColorAttr {TColorDesired(uchar(0x0)),
                                   TColorDesired(uchar(0x3))};
        c.headerFg   = TColorAttr {TColorDesired(uchar(0xD)), biosBg};
        c.blocked    = TColorAttr {TColorDesired(uchar(0xE)), biosBg};
        c.pass       = TColorAttr {TColorDesired(uchar(0xA)), biosBg};
        return c;
    }
    TColorRGB rgb = bg.asRGB();
    auto lighten = [] (TColorRGB v, int d) {
        auto up = [d] (int ch) { return uchar(ch + d > 255 ? 255 : ch + d); };
        return TColorRGB(up(v.r), up(v.g), up(v.b));
    };
    c.normal     = TColorAttr {TColorRGB(0xCBD6F2), bg};
    c.focusedRow = TColorAttr {TColorRGB(0xFFFFFF), lighten(rgb, 26)};
    c.headerFg   = TColorAttr {TColorRGB(0x9D7CD8), bg}; // the spec purple
    c.blocked    = TColorAttr {TColorRGB(0xEAC78A), bg};
    c.pass       = TColorAttr {TColorRGB(0x9CDC8C), bg};
    return c;
}

// Pad or trim a UTF-8-unaware ASCII-ish cell to an exact width. Frontmatter
// values are user text but overwhelmingly ASCII; a long value is trimmed.
std::string cell(const std::string &s, int w)
{
    std::string out = s;
    if ((int) out.size() > w)
        out.resize(w);
    else
        out.append(w - out.size(), ' ');
    return out;
}

} // namespace

/* SpecListView -------------------------------------------------------- */

SpecListView::SpecListView(const TRect &bounds, TScrollBar *vsb,
                           SpecManagerWindow &aMgr) noexcept :
    TListViewer(bounds, 1, nullptr, vsb),
    mgr(aMgr)
{
    growMode = gfGrowHiX | gfGrowHiY;
}

void SpecListView::draw()
{
    RowColors colors = rowColors(owner);
    bool winActive = owner && (owner->state & sfActive);
    bool focusedView = winActive && (state & sfSelected) != 0;
    int titleW = size.x - (kStatusW + kDomainW + kUpdatedW + kPlanW + 4);
    if (titleW < 8)
        titleW = 8;
    for (int y = 0; y < size.y; ++y)
    {
        int idx = topItem + y;
        TDrawBuffer b;
        TColorAttr c = colors.normal;
        bool isFocused = focusedView && idx == focused;
        if (isFocused)
            c = colors.focusedRow;
        b.moveChar(0, ' ', c, size.x);
        if (idx >= 0 && idx < (int) mgr.visible.size())
        {
            const SpecInfo &s = mgr.specs[mgr.visible[idx]];
            std::string status = s.status.empty() ? "draft" : s.status;
            // A reviewed spec that the gate still blocks (deps/questions) is
            // the one non-obvious state: flag it in the status cell.
            if (status == "reviewed" &&
                !specGateBlockers(s, mgr.specs).empty())
                status += "!";
            std::string plan = s.planTotal
                ? std::to_string(s.planDone) + "/" + std::to_string(s.planTotal)
                : "-";
            std::string row;
            row += cell(s.title.empty() ? s.refName() : s.title, titleW);
            row += ' ';
            row += cell(status, kStatusW);
            row += ' ';
            row += cell(s.domain, kDomainW);
            row += ' ';
            row += cell(s.updated, kUpdatedW);
            row += ' ';
            row += cell(plan, kPlanW);
            b.moveStr(0, row.c_str(), c);
        }
        else if (idx == 0 && mgr.visible.empty())
        {
            const char *msg = mgr.specs.empty()
                ? "No specs yet. Press N to create one."
                : "No specs match the filter. Press F or / to change it.";
            b.moveStr(0, msg, c);
        }
        writeLine(0, y, size.x, 1, b);
    }
}

void SpecListView::focusItem(short item)
{
    TListViewer::focusItem(item);
    if (mgr.gateLine)
        mgr.gateLine->drawView();
}

void SpecListView::handleEvent(TEvent &ev)
{
    if (ev.what == evMouseDown && (ev.mouse.eventFlags & meDoubleClick))
    {
        mgr.openFocused();
        clearEvent(ev);
        return;
    }
    if (ev.what == evKeyDown)
    {
        char ch = (char) std::tolower((unsigned char) ev.keyDown.charScan.charCode);
        if (ev.keyDown.keyCode == kbEnter)
        {
            mgr.openFocused();
            clearEvent(ev);
            return;
        }
        if (ch == 'n') { mgr.onNewSpec ? mgr.onNewSpec() : void();
                         clearEvent(ev); return; }
        if (ch == 'd')
        {
            if (const SpecInfo *s = mgr.focusedSpec())
                if (mgr.onDiscuss)
                    mgr.onDiscuss(s->path);
            clearEvent(ev);
            return;
        }
        if (ch == 'w')
        {
            if (const SpecInfo *s = mgr.focusedSpec())
                if (mgr.onDraft)
                    mgr.onDraft(s->path);
            clearEvent(ev);
            return;
        }
        if (ch == 'r') { mgr.markReviewedFocused(); clearEvent(ev); return; }
        if (ch == 'f') { mgr.cycleStatusFilter();   clearEvent(ev); return; }
        if (ch == '/') { mgr.promptTextFilter();    clearEvent(ev); return; }
    }
    TListViewer::handleEvent(ev);
}

/* Header / gate lines -------------------------------------------------- */

SpecHeaderLine::SpecHeaderLine(const TRect &bounds,
                               SpecManagerWindow &aMgr) noexcept :
    TView(bounds), mgr(aMgr)
{
    growMode = gfGrowHiX;
}

void SpecHeaderLine::draw()
{
    RowColors colors = rowColors(owner);
    int titleW = size.x - (kStatusW + kDomainW + kUpdatedW + kPlanW + 4);
    if (titleW < 8)
        titleW = 8;
    std::string row;
    row += cell("Title", titleW);
    row += ' ';
    row += cell("Status", kStatusW);
    row += ' ';
    row += cell("Domain", kDomainW);
    row += ' ';
    row += cell("Updated", kUpdatedW);
    row += ' ';
    row += cell("Plan", kPlanW);
    TDrawBuffer b;
    b.moveChar(0, ' ', colors.headerFg, size.x);
    b.moveStr(0, row.c_str(), colors.headerFg);
    writeLine(0, 0, size.x, 1, b);
}

SpecGateLine::SpecGateLine(const TRect &bounds,
                           SpecManagerWindow &aMgr) noexcept :
    TView(bounds), mgr(aMgr)
{
    growMode = gfGrowHiX | gfGrowLoY | gfGrowHiY;
}

void SpecGateLine::draw()
{
    RowColors colors = rowColors(owner);
    TDrawBuffer b;
    b.moveChar(0, ' ', colors.normal, size.x);
    std::string text;
    TColorAttr c = colors.normal;
    if (const SpecInfo *s = mgr.focusedSpec())
    {
        auto blockers = specGateBlockers(*s, mgr.specs);
        if (blockers.empty())
        {
            text = "Gate: PASS - ready to implement";
            c = colors.pass;
        }
        else
        {
            text = "Blocked: ";
            for (size_t i = 0; i < blockers.size(); ++i)
            {
                if (i)
                    text += "; ";
                text += blockers[i];
            }
            c = colors.blocked;
        }
    }
    if (!mgr.statusFilter.empty() || !mgr.textFilter.empty())
    {
        text += "  [filter:";
        if (!mgr.statusFilter.empty())
            text += " " + mgr.statusFilter;
        if (!mgr.textFilter.empty())
            text += " '" + mgr.textFilter + "'";
        text += "]";
    }
    // The key legend anchors discoverability, so it always wins the space
    // fight; the gate text truncates to what remains.
    std::string legend = "N new  W draft  D discuss  R review  F/ filter";
    int lx = size.x - (int) legend.size();
    if (lx > 8)
    {
        if ((int) text.size() > lx - 2)
            text.resize(lx - 2);
        b.moveStr(0, text.c_str(), c);
        b.moveStr(lx, legend.c_str(), colors.normal);
    }
    else
        b.moveStr(0, text.c_str(), c);
    writeLine(0, 0, size.x, 1, b);
}

/* SpecManagerWindow ---------------------------------------------------- */

SpecManagerWindow::SpecManagerWindow(const TRect &bounds, std::string aSpecsDir,
                                     SpecManagerWindow **aBackPtr) noexcept :
    TWindowInit(&TWindow::initFrame),
    TWindow(bounds, "Specs", wnNoNumber),
    specsDir(std::move(aSpecsDir)),
    backPtr(aBackPtr)
{
    options |= ofFirstClick;
    TRect r = getExtent();
    r.grow(-1, -1);
    header = new SpecHeaderLine(TRect(r.a.x, r.a.y, r.b.x, r.a.y + 1), *this);
    insert(header);
    gateLine = new SpecGateLine(TRect(r.a.x, r.b.y - 1, r.b.x, r.b.y), *this);
    insert(gateLine);
    TScrollBar *vsb = standardScrollBar(sbVertical | sbHandleKeyboard);
    list = new SpecListView(TRect(r.a.x, r.a.y + 1, r.b.x, r.b.y - 1), vsb,
                            *this);
    insert(list);
    list->select();
    refresh();
}

void SpecManagerWindow::shutDown()
{
    if (backPtr)
        *backPtr = nullptr;
    header = nullptr;
    gateLine = nullptr;
    list = nullptr;
    TWindow::shutDown();
}

void SpecManagerWindow::setState(ushort aState, Boolean enable)
{
    TWindow::setState(aState, enable);
    if (aState == sfActive)
        redraw(); // colours track the active state
}

void SpecManagerWindow::sizeLimits(TPoint &min, TPoint &max)
{
    TWindow::sizeLimits(min, max);
    min.x = 56;
    min.y = 8;
}

void SpecManagerWindow::refresh()
{
    specs = scanSpecsDir(specsDir);
    applyFilters();
}

void SpecManagerWindow::applyFilters() noexcept
{
    visible.clear();
    for (size_t i = 0; i < specs.size(); ++i)
    {
        const SpecInfo &s = specs[i];
        std::string status = s.status.empty() ? "draft" : s.status;
        if (!statusFilter.empty() && status != statusFilter)
            continue;
        if (!textFilter.empty())
        {
            auto containsCI = [] (const std::string &hay, const std::string &needle) {
                auto it = std::search(hay.begin(), hay.end(),
                                      needle.begin(), needle.end(),
                                      [] (char a, char b) {
                                          return std::tolower((unsigned char) a) ==
                                                 std::tolower((unsigned char) b);
                                      });
                return it != hay.end();
            };
            if (!containsCI(s.title, textFilter) &&
                !containsCI(s.domain, textFilter) &&
                !containsCI(status, textFilter) &&
                !containsCI(s.refName(), textFilter))
                continue;
        }
        visible.push_back(i);
    }
    if (list)
    {
        short focusedBefore = list->focused;
        list->setRange((short) visible.size());
        if (focusedBefore >= (short) visible.size())
            list->focusItem(visible.empty() ? 0 : (short) visible.size() - 1);
    }
    redraw();
}

const SpecInfo *SpecManagerWindow::focusedSpec() const noexcept
{
    if (!list || list->focused < 0 ||
        list->focused >= (short) visible.size())
        return nullptr;
    return &specs[visible[list->focused]];
}

void SpecManagerWindow::openFocused()
{
    if (const SpecInfo *s = focusedSpec())
        if (onOpen)
            onOpen(s->path);
}

void SpecManagerWindow::markReviewedFocused()
{
    const SpecInfo *s = focusedSpec();
    if (!s)
        return;
    std::string name = s->title.empty() ? s->refName() : s->title;
    std::string status = s->status.empty() ? "draft" : s->status;
    if (status == "reviewed")
        return;
    if (messageBox(mfConfirmation | mfYesButton | mfNoButton,
                   "Mark '%s' as reviewed?", name.c_str()) != cmYes)
        return;
    std::ifstream in(s->path, std::ios::binary);
    if (!in)
    {
        messageBox(mfError | mfOKButton, "Cannot read '%s'.", s->path.c_str());
        return;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    in.close();
    std::string date = today();
    std::string text = withFrontmatterValue(ss.str(), "status", "reviewed");
    text = withFrontmatterValue(text, "updated", date);
    // The review is a ledger event (D16/D18). Reviewing a spec the agent has
    // not marked ready is the user's prerogative, but recorded as such (D8).
    std::string entry = "- **R (" + date + "):** Reviewed by the user via the "
                        "Spec Manager.";
    if (status != "ready")
        entry += " (Override: status was '" + status + "'.)";
    text = appendToSpecSection(text, "Decisions", entry);
    std::ofstream out(s->path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        messageBox(mfError | mfOKButton, "Cannot write '%s'.", s->path.c_str());
        return;
    }
    out << text;
    out.close();
    refresh();
}

void SpecManagerWindow::cycleStatusFilter()
{
    static const char *kCycle[] = {"", "draft", "ready", "reviewed",
                                   "implementing", "implemented", "parked"};
    size_t i = 0;
    for (; i < sizeof kCycle / sizeof *kCycle; ++i)
        if (statusFilter == kCycle[i])
            break;
    statusFilter = kCycle[(i + 1) % (sizeof kCycle / sizeof *kCycle)];
    applyFilters();
}

void SpecManagerWindow::promptTextFilter()
{
    char buf[64];
    strncpy(buf, textFilter.c_str(), sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';
    if (inputBox("Filter Specs", "~T~ext (empty for all):", buf,
                 sizeof buf - 1) == cmOK)
    {
        textFilter = buf;
        applyFilters();
    }
}
