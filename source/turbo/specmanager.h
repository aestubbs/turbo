#ifndef TURBO_SPECMANAGER_H
#define TURBO_SPECMANAGER_H

#define Uses_TWindow
#define Uses_TListViewer
#define Uses_TScrollBar
#include <tvision/tv.h>

#include <turbo/specmodel.h>

#include <functional>
#include <string>
#include <vector>

struct SpecManagerWindow;

// The table body: one row per (filtered) spec — Title | Status | Domain |
// Updated | Plan — with the row actions (FR17): Enter opens, N new spec,
// R mark reviewed, F cycle status filter, / text filter.
struct SpecListView : public TListViewer
{
    SpecManagerWindow &mgr;

    SpecListView(const TRect &bounds, TScrollBar *vsb,
                 SpecManagerWindow &aMgr) noexcept;

    void draw() override;
    void focusItem(short item) override;
    void handleEvent(TEvent &ev) override;
};

// Column headers above the table.
struct SpecHeaderLine : public TView
{
    SpecManagerWindow &mgr;
    SpecHeaderLine(const TRect &bounds, SpecManagerWindow &aMgr) noexcept;
    void draw() override;
};

// The focused row's implementation-gate verdict (FR20) plus active filters
// and the key legend, at the bottom of the window.
struct SpecGateLine : public TView
{
    SpecManagerWindow &mgr;
    SpecGateLine(const TRect &bounds, SpecManagerWindow &aMgr) noexcept;
    void draw() override;
};

// The Spec Manager (FR16–FR20 of specs/spec-workbench.md): a singleton
// window listing every spec under <project-root>/specs — the ALM surface of
// the methodology. Toggled by cmSpecManager (Alt-P). Structured metadata
// (frontmatter, plan progress, gate state) is displayed here rather than
// read raw in the editor (D11).
struct SpecManagerWindow : public TWindow
{
    // Wired by the app after construction; the window stays decoupled from
    // TurboApp (the OutputView::onActivate pattern).
    std::function<void(const std::string &path)> onOpen;
    std::function<void()> onNewSpec;
    std::function<void(const std::string &path)> onDiscuss; // -> Workbench
    std::function<void(const std::string &path)> onDraft;   // autonomous draft
    std::function<void(const std::string &path)> onImplement; // gated handoff

    std::string specsDir;
    std::vector<turbo::SpecInfo> specs;  // scanned, newest first
    std::vector<size_t> visible;         // indices into 'specs' after filters
    std::string textFilter;              // substring filter ("" = off)
    std::string statusFilter;            // exact status ("" = all)

    SpecListView *list {nullptr};
    SpecHeaderLine *header {nullptr};
    SpecGateLine *gateLine {nullptr};
    SpecManagerWindow **backPtr {nullptr};

    SpecManagerWindow(const TRect &bounds, std::string aSpecsDir,
                      SpecManagerWindow **aBackPtr) noexcept;

    void refresh();                       // rescan disk, refilter, repaint
    void applyFilters() noexcept;
    const turbo::SpecInfo *focusedSpec() const noexcept;
    void openFocused();
    void markReviewedFocused();
    void cycleStatusFilter();
    void promptTextFilter();

    void shutDown() override;
    // Chrome resolved through the shared spec palette (speccolors.h).
    TColorAttr mapColor(uchar index) noexcept override;
    void setState(ushort aState, Boolean enable) override;
    void sizeLimits(TPoint &min, TPoint &max) override;
};

#endif // TURBO_SPECMANAGER_H
