#include "doctree.h"
#include "editwindow.h"
#include "app.h"
#include "gitclient.h"
#include <utility>
#include <vector>
#include <filesystem>
#include <turbo/tpath.h>

#define Uses_TProgram
#define Uses_TFrame
#define Uses_TDrawBuffer
#define Uses_TKeys
#define Uses_TEvent
#define Uses_TMenuItem
#define Uses_TInputLine
#include <tvision/tv.h>

#include <turbo/basicwindow.h> // shared editor-window chrome scheme

#include <cctype>

using Node = DocumentTreeView::Node;

namespace {

// Minimum query length before filtering kicks in: filtering starts once the
// query is *longer than* three characters (i.e. 4+).
constexpr size_t kFilterMinLen = 3;

// Case-insensitive substring test: does 'haystack' contain 'needleLower'
// (which is assumed already lowercased)?
bool containsCI(const char *haystack, const std::string &needleLower) noexcept
{
    if (needleLower.empty())
        return true;
    if (!haystack)
        return false;
    std::string h;
    for (const char *p = haystack; *p; ++p)
        h += (char) std::tolower((unsigned char) *p);
    return h.find(needleLower) != std::string::npos;
}

// Single-line search box on the file-tree window. Live-filters as you type and
// clears (returning focus to the tree) on Esc.
struct TreeFilterInputLine : public TInputLine
{
    DocumentTreeWindow *win {nullptr};

    TreeFilterInputLine(const TRect &bounds, DocumentTreeWindow *aWin) noexcept :
        TInputLine(bounds, 256),
        win(aWin)
    {
    }

    void handleEvent(TEvent &ev) override
    {
        if (ev.what == evKeyDown && ev.keyDown.keyCode == kbEsc)
        {
            if (win)
                win->clearFilterAndFocusTree();
            clearEvent(ev);
            return;
        }
        bool key = (ev.what == evKeyDown);
        TInputLine::handleEvent(ev);
        // After the base class has applied the keystroke, push the (possibly
        // changed) text into the tree filter. setFilter() no-ops if unchanged.
        if (key && win)
            win->applyFilterFromBox();
    }
};

} // namespace

namespace {

// Depth-first search over the WHOLE tree, including collapsed branches. The
// inherited TOutlineViewer::firstThat only visits expanded (on-screen) nodes,
// which is right for display-position work (focus/scroll) but wrong for looking
// a node up by path or editor: those must succeed regardless of what the user
// has expanded -- otherwise git badges and open-file links would vanish for any
// folder that happens to be collapsed.
template <class Pred>
Node *findNodeRec(TNode *list, Pred &&pred) noexcept
{
    for (TNode *n = list; n; n = n->next)
    {
        if (pred((Node *) n))
            return (Node *) n;
        if (n->childList)
            if (Node *r = findNodeRec(n->childList, pred))
                return r;
    }
    return nullptr;
}

} // namespace

DocumentTreeView::DocumentTreeView(const TRect &bounds, TScrollBar *hsb,
                                   TScrollBar *vsb, TNode *aRoot) noexcept :
    TOutline(bounds, hsb, vsb, aRoot)
{
    // The inner outline view is the one that swallows the first click of a
    // double-click (TView::handleEvent clears it when the view is selectable
    // but lacks ofFirstClick), so without this a double-click never reaches
    // selected() and a file won't open. The enclosing window sets ofFirstClick
    // too, but it is this view that does the swallowing.
    options |= ofFirstClick;
}

Node::Node(Node *parent, std::string_view p, bool isDir) noexcept :
    TNode(TPath::basename(p)),
    ptr(nullptr),
    parent(parent),
    path(p),
    isDir(isDir),
    editor(nullptr),
    kind(isDir ? NodeKind::Dir : NodeKind::File)
{
}

void Node::setEditor(EditorWindow *w) noexcept
{
    editor = w;
    refreshText();
}

std::string Node::targetDir() const noexcept
{
    // A home's own 'path' is a label, so the real directory is the one it stands for.
    if (kind == NodeKind::LuaHome || kind == NodeKind::SkillsHome)
        return primaryPath;
    if (isDir)
        return path;                        // Project / Dir / Skill: create inside it
    TStringView d = TPath::dirname(path);   // a file: create alongside it
    return std::string(d.data(), d.size());
}

void Node::refreshText() noexcept
{
    // A home's label has no '/', so basename() returns it whole.
    TStringView bn = TPath::basename(path);
    std::string label(bn.data(), bn.size());
    // Mark files with unsaved changes.
    if (editor && !editor->getEditor().inSavePoint())
        label += " *";
    // NOTE: the git status badge is deliberately NOT part of the label. It is
    // painted by drawNode() into a column locked to the right edge of the pane, so
    // that a long file name can neither clip it nor scroll it out of view. Keeping
    // it out of 'text' also keeps the sort key (putNode sorts on text) stable when
    // a file's status changes.
    delete[] text;
    text = newStr(label);
}

void Node::remove() noexcept
{
    if (next)
        ((Node *) next)->ptr = ptr;
    if (ptr)
        *ptr = next;
    next = nullptr;
    ptr = nullptr;
    parent = nullptr;
}

void Node::dispose() noexcept
{
    remove();
    delete this;
}

// Directories shall appear before files, both sorted alphabetically.
enum NodeType { ntDir, ntFile };
using NodeKey = std::pair<NodeType, std::string_view>;

static NodeKey getKey(Node *node) noexcept
{
    return {node->isDir ? ntDir : ntFile, node->text};
}

static void putNode(TNode **indirect, Node *node) noexcept
// Pre: node->parent is properly set.
{
    auto key = getKey(node);
    TNode *other;
    while ((other = *indirect))
    {
        if (key < getKey((Node *) other))
        {
            node->next = other;
            ((Node *) other)->ptr = &node->next;
            break;
        }
        indirect = &other->next;
    }
    *indirect = node;
    node->ptr = indirect;
}

// Directories that already have their own synthetic home at the top of the tree,
// and so must never ALSO be listed under the project node. Scoped to the project
// root, so a nested folder of the same name elsewhere is unaffected.
//
// This cannot be derived from luaSections/skillSections: openProject() scans the
// project (scanDirectory) BEFORE it builds the homes (refreshSkillsInTree), so at
// scan time those vectors are still empty.
// Lexical path equality that is separator-agnostic. On Windows the tree's paths
// come from std::filesystem (backslashes) while the literals below use forward
// slashes; a raw string compare would never match, so turbo-scripts/.claude
// would be listed twice. path comparison treats '/' and '\\' as equivalent
// separators on Windows and normalises trailing slashes, without touching disk.
static bool samePath(const std::string &a, const std::string &b) noexcept
{
    return std::filesystem::path(a).lexically_normal() ==
           std::filesystem::path(b).lexically_normal();
}

static bool hasOwnHome(const std::string &full, bool isDir,
                       const std::string &projectRoot) noexcept
{
    if (!isDir || projectRoot.empty())
        return false;
    return samePath(full, projectRoot + "/turbo-scripts")     // -> the "Project Lua" home
        || samePath(full, projectRoot + "/.claude/skills");   // -> the "Project Skills" home
}

// Safety bounds for the eager recursive project scan. Without these a deep
// tree, or a symlink/junction cycle (common on Windows reparse points), would
// recurse until the (1 MB on Windows) stack overflows and the process crashes
// the moment a project is opened. kMaxScanDepth caps recursion; kMaxScanNodes
// caps total pre-scanned nodes so a pathologically large tree can't exhaust
// memory inside this noexcept function.
static const int kMaxScanDepth = 64;
static const long kMaxScanNodes = 500000;

static void scanInto(Node *parent, TNode **list, const std::string &dirPath,
                     int depth, bool showHidden, const std::string &projectRoot,
                     long &nodeBudget) noexcept
{
    if (depth > kMaxScanDepth || nodeBudget <= 0)
        return;
    std::error_code ec;
    std::filesystem::directory_iterator it(dirPath, ec), end;
    if (ec)
        return;
    for (; it != end; it.increment(ec))
    {
        if (ec)
            break;
        if (nodeBudget <= 0)
            break; // safety cap reached: stop rather than risk exhausting memory
        const auto &entry = *it;
        std::string name = entry.path().filename().string();
        // Hidden entries (dotfiles and dot-directories such as .git) are skipped
        // unless the user has opted to show them. The .git directory and Turbo's
        // own .turbo project folder stay hidden either way (noise, not source).
        if (name.empty())
            continue;
        if (name[0] == '.' && (!showHidden || name == ".git" || name == ".turbo"))
            continue;
        std::error_code ec2, ec3;
        bool isDir = entry.is_directory(ec2);
        // A symlink/junction is shown as a folder but never descended into: its
        // target may live outside the project or form a cycle back into it.
        bool isSymlink = entry.is_symlink(ec3);
        std::string full = entry.path().string();
        if (hasOwnHome(full, isDir, projectRoot))
            continue;
        auto *node = new Node(parent, full, isDir);
        --nodeBudget;
        // The project's specs/ directory is the spec home: purple affordance
        // so the methodology's artifacts are recognisable at a glance.
        if (isDir && !projectRoot.empty() &&
            samePath(full, projectRoot + "/specs"))
            node->kind = NodeKind::Specs;
        if (isDir)
            // Every directory starts collapsed (including the top level), so the
            // tree opens compact; the user expands what they want, like any
            // other folder. (depth is no longer used to force expansion.)
            node->expanded = False;
        putNode(list, node);
        if (isDir && !isSymlink)
            scanInto(node, &node->childList, full, depth + 1, showHidden, projectRoot,
                     nodeBudget);
    }
}

void DocumentTreeView::assembleRoot() noexcept
{
    // Relink the top-level nodes into 'root' in fixed display order: the project
    // folder first, then the Lua-script homes (in luaSections order). Each may be
    // absent. The members already exist; this only (re)links the chain.
    root = nullptr;
    TNode **tail = &root;
    auto append = [&tail] (Node *n) {
        if (!n)
            return;
        n->next = nullptr;
        *tail = n;
        n->ptr = tail;
        tail = &n->next;
    };
    append(projectNode);
    for (Node *g : luaGroups)
        append(g);
    for (Node *g : skillGroups)
        append(g);
}

void DocumentTreeView::disposeProjectNode() noexcept
{
    if (!projectNode)
        return;
    disposeNode(projectNode->childList); // free the whole scanned file subtree
    projectNode->childList = nullptr;
    projectNode->dispose();              // unlink from root + delete the wrapper
    projectNode = nullptr;
}

void DocumentTreeView::rebuildProjectNode() noexcept
{
    disposeProjectNode();
    if (rootPath.empty())
        return; // no project open: only the Lua homes occupy the tree
    projectNode = new Node(nullptr, rootPath, true);
    projectNode->kind = NodeKind::Project;
    projectNode->expanded = True; // open so the project's files are visible
    long nodeBudget = kMaxScanNodes;
    scanInto(projectNode, &projectNode->childList, rootPath, 1, showHidden, rootPath,
             nodeBudget);
}

void DocumentTreeView::scanDirectory(std::string_view aRootPath) noexcept
{
    rootPath = std::string {aRootPath};
    rebuildProjectNode();
    reinjectLuaNodes(); // rebuilds the Lua homes and reassembles root
    reinjectSkillNodes(); // rebuilds the Skills homes and reassembles root
    update();
    drawView();
}

// An immediate child of a Skills home that is a directory holding a SKILL.md IS a
// skill: it keeps its real path (so its whole contents stay browsable) and carries
// the SKILL.md as the file it opens.
static void tagSkill(Node *n) noexcept
{
    if (!n || !n->isDir)
        return;
    std::string md = n->path + "/SKILL.md";
    std::error_code ec;
    if (std::filesystem::is_regular_file(md, ec))
    {
        n->kind = NodeKind::Skill;
        n->primaryPath = std::move(md);
    }
}

void DocumentTreeView::disposeGroup(Node *&group) noexcept
{
    if (!group)
        return;
    // disposeNode recurses over childList AND next, so it frees the whole subtree.
    // (Freeing only the group's direct children -- as this used to -- was harmless
    // when a home held flat Lua leaves, but leaks every skill's contents now that
    // a skills home holds real folders.)
    if (group->childList)
        disposeNode(group->childList);
    group->childList = nullptr;
    group->dispose(); // unlink from root + delete the group itself
    group = nullptr;
}

void DocumentTreeView::reinjectLuaNodes() noexcept
{
    for (Node *&g : luaGroups)
        disposeGroup(g);
    luaGroups.clear();
    if (showLuaScripts)
    {
        for (const auto &sec : luaSections)
        {
            // Synthetic directory node: its path is the label (shown as the home
            // title), with the real scripts dir carried in primaryPath. Built even
            // when empty, so the home is a clear place to drop scripts into.
            auto *group = new Node(nullptr, sec.label, true);
            group->kind = NodeKind::LuaHome;
            group->expanded = True; // open so the scripts show at a glance
            group->primaryPath = sec.dir;
            for (const auto &p : sec.scripts)
                putNode(&group->childList, new Node(group, p, false));
            luaGroups.push_back(group);
        }
    }
    assembleRoot();
    if (filtering())
        recomputeVisibility();
    update();
    drawView();
}

void DocumentTreeView::setLuaScripts(bool show, std::vector<LuaSection> sections) noexcept
{
    showLuaScripts = show;
    luaSections = std::move(sections);
    reinjectLuaNodes();
}

void DocumentTreeView::reinjectSkillNodes() noexcept
{
    for (Node *&g : skillGroups)
        disposeGroup(g);
    skillGroups.clear();
    if (showSkills)
    {
        for (const auto &sec : skillSections)
        {
            // Synthetic directory node: its path is the label, with the real skills
            // dir carried in primaryPath. Built even when the directory is empty or
            // missing, so the home stays a clear place to add a skill.
            auto *group = new Node(nullptr, sec.label, true);
            group->kind = NodeKind::SkillsHome;
            group->expanded = True;
            group->primaryPath = sec.dir;
            // Scanned like any other folder -- so a skill's SKILL.md, references/
            // and scripts/ are all reachable, instead of the skill being faked as a
            // single leaf pointing at its SKILL.md.
            long nodeBudget = kMaxScanNodes;
            scanInto(group, &group->childList, sec.dir, 1, showHidden, rootPath, nodeBudget);
            for (Node *c = (Node *) group->childList; c; c = (Node *) c->next)
                tagSkill(c);
            skillGroups.push_back(group);
        }
    }
    assembleRoot();
    if (filtering())
        recomputeVisibility();
    update();
    drawView();
}

void DocumentTreeView::setSkills(bool show, std::vector<SkillSection> sections) noexcept
{
    showSkills = show;
    skillSections = std::move(sections);
    reinjectSkillNodes();
}

void DocumentTreeView::clear() noexcept
{
    // Free the project subtree (the Lua homes are freed and rebuilt by
    // reinjectLuaNodes), forget the project path, then reassemble the root so the
    // tree shows nothing but the Lua homes (e.g. the user's global scripts).
    disposeProjectNode();
    foc = 0;
    rootPath.clear();
    reinjectLuaNodes();
    reinjectSkillNodes();
    update();
    drawView();
}

void DocumentTreeView::setShowHidden(bool show) noexcept
{
    if (show == showHidden)
        return;
    showHidden = show;
    if (rootPath.empty())
        return; // not scanned yet; the flag will take effect on the first scan
    // Remember which editors are open so we can re-link them after the rebuild
    // (rebuilding frees every node, dropping the old editor links).
    std::vector<EditorWindow *> openEditors;
    firstThat([&] (Node *node, int) {
        if (node->editor)
            openEditors.push_back(node->editor);
        return false; // visit all
    });
    foc = 0;
    rebuildProjectNode();
    reinjectLuaNodes(); // reassembles root (project + Lua homes) before re-linking
    reinjectSkillNodes(); // ... and the Skills homes
    for (auto *w : openEditors)
        linkEditor(w);
    update();
    drawView();
}

DocumentTreeView::Node *DocumentTreeView::findDir(std::string_view path) noexcept
{
    return findNodeRec(root, [path] (Node *node) {
        return node->isDir && node->path == path;
    });
}

void DocumentTreeView::refreshNode(std::string_view path) noexcept
{
    if (auto *n = findByPath(path))
    {
        n->refreshText();
        update();
        drawView();
    }
}

void DocumentTreeView::removeNode(std::string_view path) noexcept
{
    Node *n = findByPath(path);
    if (!n)
        n = findDir(path);
    if (n)
    {
        // A directory's children must go too: Node::dispose() only unlinks and
        // deletes the node itself, so freeing a folder without this leaks its
        // whole subtree.
        if (n->childList)
        {
            disposeNode(n->childList);
            n->childList = nullptr;
        }
        // A SKILL.md going away demotes its skill back to an ordinary folder.
        Node *p = n->parent;
        bool wasSkillMd = !n->isDir && TPath::basename(n->path) == TStringView("SKILL.md")
                          && p && p->kind == NodeKind::Skill;
        n->dispose();
        if (wasSkillMd)
        {
            p->kind = NodeKind::Dir;
            p->primaryPath.clear();
        }
        if (filtering())
            recomputeVisibility(); // keep the filtered view in sync
        update();
        drawView();
    }
}

DocumentTreeView::Node *DocumentTreeView::findContainer(std::string_view dirPath) noexcept
{
    if (Node *n = findDir(dirPath))
        return n;
    // A home's own 'path' is a display label, so findDir() cannot see it; match on
    // the real directory it stands for instead.
    for (Node *g : luaGroups)
        if (g->primaryPath == dirPath)
            return g;
    for (Node *g : skillGroups)
        if (g->primaryPath == dirPath)
            return g;
    return nullptr;
}

void DocumentTreeView::addNode(std::string_view path, bool isDir) noexcept
{
    // Already present, or a hidden entry we deliberately don't show.
    if (findByPath(path) || (isDir && findDir(path)))
        return;
    TStringView base = TPath::basename(path);
    if (base.empty())
        return;
    // Honour the show-hidden setting (mirrors scanInto); .git and .turbo stay hidden.
    if (base[0] == '.' && (!showHidden || base == ".git" || base == ".turbo"))
        return;
    // Mirrors scanInto: turbo-scripts and .claude/skills have their own homes at
    // the top of the tree, so never list them under the project as well.
    if (hasOwnHome(std::string(path), isDir, rootPath))
        return;
    // Locate the parent list: the root list for a top-level entry, else the
    // parent directory's child list (only if that directory is in the tree).
    TStringView dir = TPath::dirname(path);
    TNode **list;
    Node *parent = nullptr;
    if (samePath(std::string(dir), rootPath))
    {
        if (!projectNode)
            return; // no project wrapper to attach a top-level entry to
        parent = projectNode;
        list = &projectNode->childList;
    }
    else
    {
        // findContainer, not findDir: a new skill or script lands inside the real
        // directory behind a synthetic home, which findDir() cannot resolve.
        parent = findContainer(dir);
        if (!parent)
            return; // parent dir not in the tree (collapsed/unknown): skip
        list = &parent->childList;
    }
    auto *node = new Node(parent, std::string(path), isDir);
    if (isDir)
    {
        long nodeBudget = kMaxScanNodes;
        scanInto(node, &node->childList, std::string(path), 1, showHidden, rootPath, nodeBudget);
    }
    // A directory dropped straight into a Skills home is a new skill...
    if (parent->kind == NodeKind::SkillsHome)
        tagSkill(node);
    // ...and a SKILL.md landing inside a folder that sits under a Skills home
    // promotes that folder, since the watcher coalesces its changed set and may
    // report the folder before the file inside it exists.
    else if (!isDir && base == TStringView("SKILL.md") && parent->parent &&
             parent->parent->kind == NodeKind::SkillsHome)
        tagSkill(parent);
    putNode(list, node);
    if (filtering())
        recomputeVisibility(); // a new node defaults to visible; reclassify it
    update();
    drawView();
}

void DocumentTreeView::toggleExpand(Node *node) noexcept
{
    if (!node || !node->isDir)
        return;
    adjust(node, Boolean(!isExpanded(node)));
    update();
    drawView();
}

void DocumentTreeView::openOrToggle(Node *node) noexcept
{
    if (!node)
        return;
    // A skill is a folder that opens: Enter/double-click goes straight to its
    // SKILL.md (the thing you almost always want), while the chevron and the
    // arrow keys still expand it to reach references/, scripts/ and the rest.
    std::string target = node->openPath();
    if (target.empty())
    {
        toggleExpand(node); // Project / Dir / a home: nothing to open
        return;
    }
    // Re-resolve rather than trust node->editor: a skill's editor is linked to its
    // SKILL.md child node, not to the skill folder the user activated.
    if (Node *n = findByPath(target); n && n->editor)
        n->editor->focus();
    else if (auto *app = (TurboApp *) TProgram::application)
        app->openFileFromTree(target.c_str());
}

void DocumentTreeView::selected(int i) noexcept
{
    // Triggered by Enter or a double-click (not by moving the highlight).
    openOrToggle((Node *) getNode(i));
}

void DocumentTreeView::handleEvent(TEvent &ev)
{
    if (ev.what == evMouseDown)
    {
        TPoint local = makeLocal(ev.mouse.where);
        int row = delta.y + local.y;
        bool onRow = row >= 0 && row < limit.y;
        // Right-click: open the context menu for the clicked node. Handle it
        // ourselves so the base outline doesn't also treat it as a select/toggle.
        if (ev.mouse.buttons & mbRightButton)
        {
            if (onRow)
            {
                foc = row;
                update();
                drawView();
                showContextMenu(row, ev.mouse.where);
            }
            clearEvent(ev);
            return;
        }
        // Left double-click: open/toggle the clicked node directly. The base
        // class would do this on meDoubleClick too, but only reliably once the
        // tree already has focus; doing it here makes a plain double-click work
        // even when arriving from another window. clearEvent() stops the base
        // from acting on the same event (which would open the file twice).
        if ((ev.mouse.buttons & mbLeftButton) &&
            (ev.mouse.eventFlags & meDoubleClick) && onRow)
        {
            foc = row;
            openOrToggle((Node *) getNode(row));
            clearEvent(ev);
            return;
        }
    }
    // Arrow-key tree navigation. The base TOutline aliases Left->Up and
    // Right->Down (plain focus movement, no horizontal scroll), so we override
    // Left/Right entirely to give them the conventional expand/collapse/navigate
    // semantics. Only act while focused; Up/Down/Enter still go to the base.
    if (ev.what == evKeyDown && (state & sfFocused))
    {
        ushort key = ev.keyDown.keyCode;
        // Ctrl-F jumps the cursor into the search box; Esc clears any filter.
        // (Intercepted here so Ctrl-F doesn't also trigger the editor's Find.)
        if (key == kbCtrlF)
        {
            if (win)
                win->focusFilter();
            clearEvent(ev);
            return;
        }
        if (key == kbEsc && filtering())
        {
            if (win)
                win->clearFilter();
            clearEvent(ev);
            return;
        }
        if (key == kbRight || key == kbLeft)
        {
            auto *node = (Node *) getNode(foc);
            if (!node)
            {
                TOutline::handleEvent(ev);
                return;
            }
            if (key == kbRight)
            {
                // Right on a collapsed directory expands it. On an already
                // expanded directory (or a file) move down one row, which lands
                // on the first child of an expanded dir. toggleExpand, not
                // openOrToggle: a skill is a directory that OPENS its SKILL.md,
                // so openOrToggle here would open the file instead of expanding.
                if (node->isDir && !isExpanded(node))
                    toggleExpand(node);
                else
                {
                    foc++;
                    update();   // update() -> adjustFocus(foc): clamps + scrolls
                    drawView();
                }
            }
            else // kbLeft
            {
                // Left on an expanded directory collapses it. Otherwise (file or
                // collapsed dir) jump focus to the parent directory's row, if any.
                if (node->isDir && isExpanded(node))
                    toggleExpand(node);
                else if (node->parent)
                {
                    Node *parent = node->parent;
                    int parentPos = -1;
                    firstThat([parent, &parentPos] (Node *n, int pos) {
                        if (n == parent) { parentPos = pos; return true; }
                        return false;
                    });
                    if (parentPos >= 0)
                    {
                        foc = parentPos;
                        update();   // scroll the parent row into view
                        drawView();
                    }
                }
                // Top-level file/collapsed dir: no parent, so do nothing.
            }
            clearEvent(ev);
            return;
        }
    }
    TOutline::handleEvent(ev);
}

// --- Name filter -----------------------------------------------------------

namespace {
// Set 'visible' on a node and its entire subtree.
void markSubtree(Node *n, bool v) noexcept
{
    n->visible = v;
    for (Node *c = (Node *) n->childList; c; c = (Node *) c->next)
        markSubtree(c, v);
}
// Does a file node satisfy the git-status filter?
bool gitFilterMatch(const Node *n, int gf) noexcept
{
    if (gf == tgfAll)
        return true;
    switch (gf)
    {
        case tgfModified:   return n->gitStatus == 'M' || n->gitStatus == 'R' ||
                                   n->gitStatus == 'D' || n->gitStatus == 'A';
        case tgfStaged:     return n->gitStaged;
        case tgfUntracked:  return n->gitStatus == '?';
        case tgfConflicted: return n->gitStatus == 'U';
    }
    return true;
}
// Compute visibility for a node and its descendants; returns whether the node
// is visible. A file is visible iff its name matches the query AND its git state
// matches the git filter. A folder is visible iff its own name matches (revealing
// its whole subtree -- name filter only) or any descendant is visible.
bool computeVisible(Node *n, const std::string &q, int gf) noexcept
{
    bool nameMatch = q.empty() || containsCI(n->text, q);
    if (!n->isDir)
    {
        bool v = nameMatch && gitFilterMatch(n, gf);
        n->visible = v;
        return v;
    }
    // The "folder name matches -> reveal its whole subtree" shortcut applies only
    // to the pure name filter; a git filter must still match descendant files.
    if (gf == tgfAll && !q.empty() && containsCI(n->text, q))
    {
        markSubtree(n, true);
        return true;
    }
    bool any = false;
    for (Node *c = (Node *) n->childList; c; c = (Node *) c->next)
        any |= computeVisible(c, q, gf);
    n->visible = any;
    return any;
}
} // namespace

void DocumentTreeView::recomputeVisibility() noexcept
{
    if (!filtering())
        return; // the overrides ignore 'visible' when no filter is active
    for (Node *n = (Node *) root; n; n = (Node *) n->next)
        computeVisible(n, filter, gitFilter);
}

void DocumentTreeView::setFilter(std::string_view query) noexcept
{
    std::string q;
    for (char c : query)
        q += (char) std::tolower((unsigned char) c);
    if (q.size() <= kFilterMinLen)
        q.clear(); // 3 chars or fewer: no filtering
    if (q == filter)
        return;    // nothing changed
    // Keep the same node highlighted across the change (so clearing the filter
    // leaves the active node where it was).
    Node *focusedNode = (Node *) getNode(foc);
    filter = std::move(q);
    recomputeVisibility();
    int pos = -1;
    if (focusedNode)
        firstThat([focusedNode, &pos] (Node *n, int p) {
            if (n == focusedNode) { pos = p; return true; }
            return false;
        });
    foc = (pos >= 0) ? pos : 0; // first visible row if the old node is now hidden
    update();    // recompute range, clamp foc, scroll it into view
    drawView();
}

void DocumentTreeView::setGitFilter(int gf) noexcept
{
    if (gf == gitFilter)
        return;
    Node *focusedNode = (Node *) getNode(foc); // keep the same node highlighted
    gitFilter = gf;
    recomputeVisibility();
    int pos = -1;
    if (focusedNode)
        firstThat([focusedNode, &pos] (Node *n, int p) {
            if (n == focusedNode) { pos = p; return true; }
            return false;
        });
    foc = (pos >= 0) ? pos : 0;
    update();
    drawView();
}

// Outline iteration overrides -- skip non-visible nodes while filtering so the
// base viewer lays out, scrolls and draws only the matching subset.

TNode *DocumentTreeView::getRoot()
{
    TNode *r = root;
    if (filtering())
        while (r && !((Node *) r)->visible)
            r = r->next;
    return r;
}

TNode *DocumentTreeView::getNext(TNode *node)
{
    TNode *n = node->next;
    if (filtering())
        while (n && !((Node *) n)->visible)
            n = n->next;
    return n;
}

TNode *DocumentTreeView::getChild(TNode *node, int i)
{
    TNode *c = node->childList;
    if (filtering())
        while (c && !((Node *) c)->visible)
            c = c->next;
    while (i-- > 0 && c)
        c = getNext(c);
    return c;
}

int DocumentTreeView::getNumChildren(TNode *node)
{
    if (!filtering())
        return TOutline::getNumChildren(node);
    int n = 0;
    for (TNode *c = getChild(node, 0); c; c = getNext(c))
        ++n;
    return n;
}

Boolean DocumentTreeView::hasChildren(TNode *node)
{
    if (!filtering())
        return TOutline::hasChildren(node);
    return Boolean(getChild(node, 0) != 0);
}

Boolean DocumentTreeView::isExpanded(TNode *node)
{
    if (filtering())
        return hasChildren(node); // show every kept folder expanded
    return TOutline::isExpanded(node);
}

void DocumentTreeView::showContextMenu(int row, TPoint where) noexcept
{
    auto *node = (Node *) getNode(row);
    if (!node)
        return;
    // Capture identity by value: the menu is modal, and while it is up the idle
    // loop can still run the filesystem watcher, which may rebuild the tree and
    // free 'node'. So below we act on these copies and re-resolve by path, never
    // touching 'node' again.
    const NodeKind kind      = node->kind;
    const std::string path   = node->path;
    const std::string primary = node->primaryPath; // a home's real dir / a skill's SKILL.md
    const std::string dir    = node->targetDir();  // where New File/Folder create
    const bool isDir         = node->isDir;
    const char gs            = node->gitStatus;

    // A home's 'path' is a display label, not a real file, so it can be neither
    // renamed, deleted, nor staged. And renaming the project root would rename the
    // open project's own directory -- surprising and risky -- so skip that too.
    const bool isHome = (kind == NodeKind::LuaHome || kind == NodeKind::SkillsHome);

    TMenuItem *items = nullptr, *tail = nullptr;
    auto add = [&] (TMenuItem *it) {
        if (!items) items = tail = it;
        else { tail->append(it); tail = it; }
    };
    auto sep = [&] { if (items) { tail->append(&newLine()); tail = tail->next; } };

    // Only a node with something to open gets "Open": a skill opens its SKILL.md,
    // while a pure container has nothing to open (activating it expands it).
    if (kind == NodeKind::File || kind == NodeKind::Skill)
        add(new TMenuItem(kind == NodeKind::Skill ? "~O~pen SKILL.md" : "~O~pen",
                          cmTreeOpen, kbNoKey, hcNoContext));
    if (kind == NodeKind::LuaHome)
        add(new TMenuItem("~N~ew Lua Script...", cmTreeNewLuaScript, kbNoKey, hcNoContext));
    if (kind == NodeKind::SkillsHome)
        add(new TMenuItem("~N~ew Skill...", cmTreeNewSkill, kbNoKey, hcNoContext));
    // 'T' rather than 'N' on a home, whose menu already spends 'N' on New Skill /
    // New Lua Script.
    add(new TMenuItem(isHome ? "New ~T~ext File..." : "~N~ew File...",
                      cmTreeNewFile, kbNoKey, hcNoContext));
    // Accelerator 'F' (not 'o') so it doesn't collide with "Open" in this menu.
    add(new TMenuItem("New ~F~older...", cmTreeNewFolder, kbNoKey, hcNoContext));

    if (!isHome && kind != NodeKind::Project)
    {
        sep();
        // Renaming a skill renames its FOLDER -- the skill's identity -- and never
        // its SKILL.md, because a skill node's path IS the directory.
        add(new TMenuItem("~R~ename...", cmTreeRename, kbNoKey, hcNoContext));
    }
    if (!isHome)
    {
        sep();
        add(new TMenuItem("Git ~A~dd", cmTreeGitAdd, kbNoKey, hcNoContext));
        // Offer Revert only for a tracked file with committed content to restore
        // from: modified ('M'), deleted ('D') or conflicted ('U'). Not for new/
        // untracked files (nothing at HEAD), renames, or directories.
        if (!isDir && (gs == 'M' || gs == 'D' || gs == 'U'))
            add(new TMenuItem("Git Re~v~ert", cmTreeGitRevert, kbNoKey, hcNoContext));
    }

    ushort cmd = popupMenu(where, *items, nullptr);

    auto *app = (TurboApp *) TProgram::application;
    if (!app)
        return;
    switch (cmd)
    {
        case cmTreeOpen:
            // Re-resolve by path (see note above) rather than reuse 'node'.
            if (Node *n = (isDir ? findDir(path) : findByPath(path)))
                openOrToggle(n);
            else
                app->openFileFromTree(
                    (kind == NodeKind::Skill ? primary : path).c_str());
            break;
        // The home actions must use the captured 'primary': a home's own path is a
        // label, so re-resolving it would find nothing.
        case cmTreeNewLuaScript: app->treeNewLuaScript(primary); break;
        case cmTreeNewSkill:     app->treeNewSkill(primary);     break;
        case cmTreeNewFile:      app->treeCreateFile(dir);       break;
        case cmTreeNewFolder:    app->treeCreateFolder(dir);     break;
        case cmTreeRename:       app->treeRenamePath(path, isDir); break;
        case cmTreeGitAdd:       app->treeStagePath(path);       break;
        case cmTreeGitRevert:    app->treeRevertPath(path);      break;
    }
}

namespace {

struct DrawCtx { TDrawBuffer *b; int last; };

enum class TreeRole { Normal, Focused, Selected };

// Row colours for the file tree, derived from the shared window-chrome scheme so
// the tree matches the editor windows (and follows any theme edits).
struct RowColors {
    TColorAttr    fill;   // the row background (and the blank tail of the row)
    TColorAttr    text;   // the node's name
    TColorAttr    guide;  // the connector graph: DIM, so the chrome recedes
    TColorDesired bg;     // the row background, for composing the icon's attr
};

RowColors treeColors(TreeRole role, bool active) noexcept
{
    using namespace turbo;
    // Normal rows track the window's active state (the unified blue when active,
    // a dimmer shade when not) -- matching the frame and the other windows.
    TColorDesired bg = ::getBack(windowSchemeActive[active ? wndFrameActive
                                                           : wndFramePassive]);
    // Classic 16-colour mode (the window scheme holds BIOS colours): use BIOS
    // colours picked to stay distinct on the blue tree background. The 24-bit
    // values below would quantise to 16 colours unpredictably -- the dim
    // grey-blue guide in particular collapses into the blue background, so the
    // connector lines vanish. Detected via the scheme's colour type, so no extra
    // plumbing of the colour-mode setting is needed. (BIOS: black0 blue1 green2
    // cyan3 gray7 darkGray8 white15.)
    if (bg.isBIOS())
    {
        auto A = [] (uchar f, uchar b) {
            return TColorAttr(TColorDesired(uchar(f)), TColorDesired(uchar(b)));
        };
        switch (role)
        {
            case TreeRole::Focused:  // the current row: dark text on cyan
                return { A(0, 3), A(0, 3), A(0, 3), TColorDesired(uchar(3)) };
            case TreeRole::Selected: // a marked row: dark text on light grey
                return { A(0, 7), A(0, 7), A(0, 7), TColorDesired(uchar(7)) };
            default:                 // light text, dim (but visible) grey guide
                return { A(7, 1), A(7, 1), A(8, 1), TColorDesired(uchar(1)) };
        }
    }
    switch (role)
    {
        case TreeRole::Focused:
            return { TColorAttr(0xFFFFFF, 0x3A7FD0), TColorAttr(0xFFFFFF, 0x3A7FD0),
                     TColorAttr(0xA8C8EC, 0x3A7FD0), TColorRGB(0x3A7FD0) };
        case TreeRole::Selected:
            return { TColorAttr(0xFFFFFF, 0x2F6FB0), TColorAttr(0xFFFFFF, 0x2F6FB0),
                     TColorAttr(0x9CBEE4, 0x2F6FB0), TColorRGB(0x2F6FB0) };
        default:
            return { TColorAttr(0xEAEFFF, bg), TColorAttr(0xDCE4F7, bg),
                     TColorAttr(0x5D6A8C, bg), bg };
    }
}

// Row prefix geometry:
//
//   [guide x level][connector][chevron][' ']( [icon][' '] )
//
// Every glyph is exactly one column, so the prefix is ALWAYS
// 'level + 3 + iconColumns' wide, and the icon's own column -- the gap after the
// chevron, then the icon -- is ALWAYS 'level + 3'. That constancy is load-bearing:
// getGraph() reserves the icon's column, and the base viewer MEASURES that string
// for the horizontal-scroll limit and for the click-to-toggle hit test. A set with
// no pictograms reports iconColumns == 0, so the column collapses rather than
// sitting there blank.
int iconColumn(int level) noexcept { return level + 3; }

// The first column of the right-hand status gutter: one blank separator, then the
// git badge hard against the pane's right edge. Reserved on EVERY row, whether or
// not it has a status, so the badges form a straight column -- and the name is
// clipped to it, which is what stops a long file name from ever covering a badge.
int gutterCol(int sizeX) noexcept { return max(0, sizeX - 2); }

// The single source of truth for the prefix.
//
// getGraph() calls this for its WIDTH alone -- that is all the base viewer uses it
// for -- while drawNode() calls it with the node in hand, so it can pick glyphs
// getGraph() structurally cannot know about: 'container' (an empty folder is still
// a folder, though the base class reports it exactly like a leaf) and 'first' (the
// tree's very first row, which rounds off the top). Both agree on the width.
//
// 'lines' bit i is set when an ancestor at level i has a following sibling, so a
// vertical guide must run down column i.
std::string treeGraph(int level, long lines, ushort flags,
                      bool first, bool container) noexcept
{
    const TreeGlyphs &g = treeGlyphs();
    std::string s;
    s.reserve(3 * (level + 2) + 3);
    long l = lines;
    for (int i = 0; i < level; ++i, l >>= 1)
        s += (l & 1) ? g.vert : " ";
    // The connector. Top-level rows get a real one too -- which is what lets the
    // old level-0 special case, and the root-line fixup that propped it up, both
    // disappear: the first root rounds off the top of the tree, the last closes
    // it, and everything between is a tee.
    if (first)                  s += g.top;
    else if (flags & ovLast)    s += g.corner;
    else                        s += g.tee;
    // The chevron. Mind the flag semantics (toutline.cpp:282-286):
    //   ovChildren = has children AND expanded
    //   ovExpanded = has NO children OR expanded   <-- set for leaves too!
    // so "collapsed with children" is !ovExpanded, and a childless folder is
    // indistinguishable from a leaf to the base class -- hence 'container'.
    if (!(flags & ovExpanded))      s += g.chevRight; // collapsed folder
    else if (flags & ovChildren)    s += g.chevDown;  // expanded folder
    else if (container)             s += g.chevRight; // empty folder: still a folder
    else                            s += g.dash;      // a leaf file
    s += " ";                                         // gap after the chevron
    if (g.iconColumns)
        s += "  ";                                    // the ICON's column, + its gap
    return s;                                         // width == graphWidth(level)
}

// Paints one row: the connector graph (dim), then the node's icon in its own
// colour, then the name. Replaces TOutlineViewer's internal drawTree(), which
// paints the whole row in a single attribute and so could not colour an icon.
Boolean drawNode( TOutlineViewer *v, TNode *cur, int level, int position,
                  long lines, ushort flags, void *arg )
{
    auto *ctx = (DrawCtx *) arg;
    TDrawBuffer &dBuf = *ctx->b;
    if (position < v->delta.y)
        return False;
    if (position >= v->delta.y + v->size.y)
        return True;

    auto *node = (Node *) cur;
    bool winActive = v->owner && (v->owner->state & sfActive);
    bool focused = (position == v->foc) && (v->state & sfFocused);
    bool selected = v->isSelected(position);
    RowColors rc = treeColors(focused  ? TreeRole::Focused
                            : selected ? TreeRole::Selected
                                       : TreeRole::Normal, winActive);
    dBuf.moveChar(0, ' ', rc.fill, v->size.x);

    // 1. The connector graph. TText's strIndent is in COLUMNS, and every glyph we
    //    emit is one column, so a horizontal scroll can never cut one in half.
    std::string graph = treeGraph(level, lines, flags,
                                  /*first*/ level == 0 && cur == v->getRoot(),
                                  /*container*/ node->isDir);
    int x = strwidth(graph) - v->delta.x;   // == graphWidth(level) - delta.x
    if (x > 0)
        dBuf.moveStr(0, graph, rc.guide, (ushort) -1U, (ushort) v->delta.x);

    // 2. The icon, painted into the column the graph reserved for it. A negative
    //    column means it has scrolled off to the left -- and must not be passed to
    //    moveStr, whose indent is a ushort.
    int iconX = iconColumn(level) - v->delta.x;
    if (iconX >= 0 && iconX < v->size.x)
    {
        // "Open" for the icon means it actually has children showing, which is what
        // makes the open-folder glyph agree with the chevron beside it.
        TreeIcon icon = treeIconFor(node->kind, node->path, (flags & ovChildren) != 0);
        if (icon.glyph && icon.glyph[0])
            dBuf.moveStr((ushort) iconX, icon.glyph,
                         icon.inheritColor ? rc.text
                                           : TColorAttr(TColorRGB(icon.color), rc.bg),
                         (ushort) (v->size.x - iconX));
    }

    // 3. The name, clipped so it can never run into the status gutter (below).
    //    Drawn at a column derived from the graph's width, so even a mis-measured
    //    icon could never shift it.
    //
    //    The name keeps its KIND colour (gold folder, violet skill, blue Lua) even
    //    when the file is dirty: git status is now carried unambiguously by the
    //    gutter chip, so tinting the whole row as well would be redundant -- and
    //    would mean a folder lost its gold the moment anything inside it changed.
    TStringView text = v->getText(cur);
    TColorAttr c = rc.text;
    uint32_t nameFg = treeNameColor(node->kind);
    if (!nameFg && !node->isDir)
        // Files inside the spec home carry its tint (FR15's secondary
        // surface): a spec is recognisably a spec in the tree too.
        for (Node *p = node->parent; p; p = p->parent)
            if (p->kind == NodeKind::Specs)
            {
                nameFg = treeNameColor(NodeKind::Specs);
                break;
            }
    if (nameFg && !focused && !selected)
        setFore(c, TColorRGB(nameFg));
    if (node->editor)
        setStyle(c, getStyle(c) | slBold);
    int nameMax = max(0, gutterCol(v->size.x) - max(0, x));
    dBuf.moveStr(max(0, x), text, c, (ushort) nameMax, (ushort) max(0, -x));

    // 4. The git status, in a column LOCKED to the right edge of the pane: it is
    //    positioned from size.x rather than from the text, and is never offset by
    //    delta.x, so no name -- however long, however far scrolled -- can clip it
    //    or push it out of view. Files get an inverted chip (the status letter
    //    knocked out of a solid colour) so it reads at a glance; a directory's
    //    rolled-up "something inside me changed" stays a quiet dot, since making
    //    every ancestor folder shout would drown out the files that actually changed.
    if (char gs = node->gitStatus; gs && v->size.x > 2)
    {
        int col = v->size.x - 1;
        if (gs == '.')
            dBuf.moveStr((ushort) col, "\xC2\xB7",          // U+00B7 middle dot
                         TColorAttr(TColorRGB(0x9AA6CE), rc.bg), 1);
        else
        {
            TColorDesired chip {};
            switch (gs)
            {
                case 'M': case 'R': chip = 0xE6C98C; break; // gold (modified/renamed)
                case 'A': case '?': chip = 0x9CDC8C; break; // green (added/untracked)
                case 'D':           chip = 0xE06C75; break; // red (deleted)
                case 'U':           chip = 0xD79CD2; break; // magenta (conflicted)
                default:            chip = 0xCBD6F2; break;
            }
            // Inverted: the status colour becomes the background, the row's own
            // background the letter. Reads as a solid chip, and stays legible on
            // the focused/selected row (whose background is the highlight blue).
            const char letter[2] = { gs, '\0' };
            dBuf.moveStr((ushort) col, letter, TColorAttr(rc.bg, chip), 1);
        }
    }

    v->writeLine(0, position - v->delta.y, v->size.x, 1, dBuf);
    ctx->last = position;
    return False;
}

} // namespace

char *DocumentTreeView::getGraph(int level, long lines, ushort flags)
{
    // The base viewer only ever MEASURES this string -- for the horizontal-scroll
    // limit and for the click-to-toggle hit test. All the drawing is done by
    // drawNode(), which has the node and so can pick the exact glyphs. Both call
    // treeGraph(), so the width they agree on is exact, which is all that matters
    // here (every glyph in every set is one column, so the placeholder arguments
    // cannot change it).
    return newStr(treeGraph(level, lines, flags, false, false));
}

void DocumentTreeView::draw()
{
    TDrawBuffer dBuf;
    DrawCtx ctx {&dBuf, -1};
    TOutlineViewer::firstThat(drawNode, &ctx);
    RowColors nrm = treeColors(TreeRole::Normal, owner && (owner->state & sfActive));
    dBuf.moveChar(0, ' ', nrm.fill, size.x);
    writeLine(0, ctx.last + 1, size.x, size.y - (ctx.last - delta.y), dBuf);
}

void DocumentTreeView::linkEditor(EditorWindow *w) noexcept
{
    if (w->filePath().empty())
        return;
    if (auto *node = findByPath(w->filePath()))
    {
        node->setEditor(w);
        update();
        drawView();
    }
}

void DocumentTreeView::unlinkEditor(EditorWindow *w) noexcept
{
    // Full-tree search (not findByEditor, which is visible-only): the link must
    // be cleared even if the file's folder is collapsed, or a closed editor's
    // dangling pointer would linger on a hidden node and crash when next drawn.
    if (auto *node = findNodeRec(root, [w] (Node *n) { return n->editor == w; }))
    {
        node->setEditor(nullptr);
        update();
        drawView();
    }
}

void DocumentTreeView::focusEditor(EditorWindow *w) noexcept
{
    // Move the highlight onto the active editor's node and scroll it into view.
    int i;
    if (findByEditor(w, &i))
    {
        foc = i;
        update();   // adjustFocus(foc) scrolls the node into view
        drawView();
    }
}

void DocumentTreeView::focusNext() noexcept
{
    // Cycle forward to the next file currently open in an editor.
    firstThat([this] (Node *node, int pos) {
        if (node->editor && pos > foc) {
            selected(pos);
            return true;
        }
        return false;
    });
}

void DocumentTreeView::focusPrev() noexcept
{
    // Cycle backward to the previous file currently open in an editor.
    int prevPos = -1;
    firstThat([this, &prevPos] (Node *node, int pos) {
        if (node->editor) {
            if (pos < foc)
                prevPos = pos;
            else if (prevPos >= 0) {
                selected(prevPos);
                return true;
            }
        }
        return false;
    });
}

static Node *findEditorNode(Node *list, EditorWindow *w) noexcept
{
    for (Node *n = list; n; n = (Node *) n->next)
    {
        if (n->editor == w)
            return n;
        if (n->childList)
            if (Node *r = findEditorNode((Node *) n->childList, w))
                return r;
    }
    return nullptr;
}

void DocumentTreeView::revealEditor(EditorWindow *w) noexcept
{
    // Search the whole tree (including collapsed branches) for the editor's
    // node, expand its ancestors so it becomes visible, then highlight it.
    Node *node = findEditorNode((Node *) root, w);
    if (!node)
        return;
    // A reveal must win over any active filter that might be hiding the node.
    if (filtering() && win)
        win->clearFilter();
    for (Node *p = node->parent; p; p = p->parent)
        p->expanded = True;
    update();
    focusEditor(w);
}

Node* DocumentTreeView::findByEditor(const EditorWindow *w, int *pos) noexcept
{
    return firstThat(
    [w, pos] (Node *node, int position)
    {
        if (node->editor && node->editor == w) {
            if (pos)
                *pos = position;
            return true;
        }
        return false;
    });
}

Node* DocumentTreeView::findByPath(std::string_view path) noexcept
{
    return findNodeRec(root, [path] (Node *node) {
        return !node->isDir && node->path == path;
    });
}

void DocumentTreeView::collectFilePaths(std::vector<std::string> &out) noexcept
{
    findNodeRec(root, [&out] (Node *node) {
        if (!node->isDir)
            out.push_back(node->path);
        return false; // visit every node
    });
}

static char badgeFor(const GitFileStatus &s) noexcept
{
    switch (s.state)
    {
        case GitFileState::Modified:   return 'M';
        case GitFileState::Added:      return 'A';
        case GitFileState::Deleted:    return 'D';
        case GitFileState::Renamed:    return 'R';
        case GitFileState::Untracked:  return '?';
        case GitFileState::Conflicted: return 'U';
        default:                       return 0;
    }
}

static void clearStatusRecursive(Node *list) noexcept
{
    for (Node *n = list; n; n = (Node *) n->next)
    {
        bool had = n->gitStatus != 0;
        n->gitStatus = 0;
        n->gitStaged = false;
        if (had)
            n->refreshText();
        if (n->childList)
            clearStatusRecursive((Node *) n->childList);
    }
}

void DocumentTreeView::applyGitStatus(
    const std::unordered_map<std::string, GitFileStatus> &files) noexcept
{
    clearStatusRecursive((Node *) root);
    for (auto &kv : files)
    {
        Node *node = findByPath(kv.first);
        if (!node)
            continue; // file outside the scanned tree (e.g. ignored dir)
        node->gitStatus = badgeFor(kv.second);
        node->gitStaged = kv.second.staged;
        node->refreshText();
        // Roll the "contains changes" marker up to ancestor directories.
        for (Node *p = node->parent; p; p = p->parent)
            if (p->gitStatus == 0)
            {
                p->gitStatus = '.';
                p->refreshText();
            }
    }
    // Keep an active git-status filter live as badges change (e.g. a file that
    // just became conflicted should appear under the Conflicted filter).
    if (filtering())
        recomputeVisibility();
    update();
    drawView();
}

namespace {

const char *gitFilterLabel(int gf) noexcept
{
    switch (gf)
    {
        case tgfModified:   return "Modified";
        case tgfStaged:     return "Staged";
        case tgfUntracked:  return "Untracked";
        case tgfConflicted: return "Conflicted";
        default:            return "All";
    }
}

// The git-status filter dropdown, a one-row control under the name filter.
// Clicking it pops a menu of states; choosing one filters the tree.
struct TreeGitFilterBar : public TView
{
    DocumentTreeWindow *win {nullptr};

    TreeGitFilterBar(const TRect &b, DocumentTreeWindow *w) noexcept :
        TView(b), win(w) { eventMask |= evMouseDown; }

    void draw() override
    {
        int gf = (win && win->tree) ? win->tree->gitFilter : tgfAll;
        TColorAttr c = win ? win->mapColor(turbo::wndInputLineNormal + 1) : TColorAttr {};
        TDrawBuffer b;
        b.moveChar(0, ' ', c, size.x);
        std::string s = "Git: " + std::string(gitFilterLabel(gf));
        b.moveStr(1, s.c_str(), c);
        if (size.x >= 3)
            b.moveStr(size.x - 2, "v", c); // dropdown affordance
        writeLine(0, 0, size.x, 1, b);
    }

    void handleEvent(TEvent &ev) override
    {
        if (ev.what == evMouseDown && win && win->tree)
        {
            static const char *labels[tgfCount] =
                {"All", "Modified", "Staged", "Untracked", "Conflicted"};
            TMenuItem *head = nullptr, *tail = nullptr;
            for (int i = 0; i < tgfCount; ++i)
            {
                auto *it = new TMenuItem(labels[i], cmTreeGitFilterBase + i,
                                         kbNoKey, hcNoContext);
                if (!head) head = tail = it;
                else { tail->append(it); tail = it; }
            }
            ushort cmd = popupMenu(ev.mouse.where, *head, nullptr);
            int sel = (int) cmd - cmTreeGitFilterBase;
            if (sel >= 0 && sel < tgfCount)
            {
                win->tree->setGitFilter(sel);
                drawView();
            }
            clearEvent(ev);
            return;
        }
        TView::handleEvent(ev);
    }
};

// The tree window's frame, customised to rule one interior row (the filter-area
// divider) with a line that joins the side borders via the proper tee
// connectors -- so it reads as part of the window chrome rather than a detached
// rule floating inside it. Drawn by the frame (not a separate view) so it stays
// correct across the frame's own redraws (activation, title change).
struct TreeFrame : public TFrame
{
    int dividerRow {-1}; // window-local row to rule, or -1 for none

    TreeFrame(const TRect &b) noexcept : TFrame(b) {}

    void draw() override
    {
        TFrame::draw();
        if (dividerRow <= 0 || dividerRow >= size.y - 1)
            return;
        // Match the border colour, then lay a single-line rule with double-line
        // tee junctions (CP437: 0xC7 = ╟, 0xC4 = ─, 0xB6 = ╢) -- the same bytes
        // TFrame uses, so they track the active/passive frame palette.
        TColorAttr c = getColor((state & sfActive) ? 0x0503 : 0x0101);
        TDrawBuffer b;
        for (int x = 0; x < size.x; ++x)
        {
            b.putChar(x, x == 0 ? '\xC7' : (x == size.x - 1 ? '\xB6' : '\xC4'));
            b.putAttribute(x, c);
        }
        writeLine(0, dividerRow, size.x, 1, b);
    }
};

} // namespace

TFrame *DocumentTreeWindow::initFrame(TRect bounds)
{
    return new TreeFrame(bounds);
}

DocumentTreeWindow::DocumentTreeWindow(const TRect &bounds, DocumentTreeWindow **ptr) noexcept :
    TWindowInit(&DocumentTreeWindow::initFrame),
    TWindow(bounds, "Files", wnNoNumber),
    ptr(ptr)
{
    // Without ofFirstClick, the first click of a double-click is swallowed just
    // to activate the window (when focus was in an editor), so the outline never
    // sees a double-click and a file won't open. Forward that first click.
    options |= ofFirstClick;
    auto *hsb = standardScrollBar(sbHorizontal),
         *vsb = standardScrollBar(sbVertical);
    // Delineated filter area at the top: name filter (row 0), git-status filter
    // dropdown (row 1), a divider (row 2, ruled by the frame); the tree below.
    TRect inner = getExtent();
    inner.grow(-1, -1);
    TRect nameR = inner; nameR.b.y = nameR.a.y + 1;
    TRect gitR  = inner; gitR.a.y = nameR.b.y;  gitR.b.y = gitR.a.y + 1;
    TRect divR  = inner; divR.a.y = gitR.b.y;   divR.b.y = divR.a.y + 1;
    TRect treeR = inner; treeR.a.y = divR.b.y;

    auto *box = new TreeFilterInputLine(nameR, this);
    box->growMode = gfGrowHiX; // full width, pinned to the top
    insert(box);
    filterBox = box;

    auto *gbar = new TreeGitFilterBar(gitR, this);
    gbar->growMode = gfGrowHiX;
    insert(gbar);

    // The divider row is ruled by the frame so the line joins the side borders.
    if (frame)
        ((TreeFrame *) frame)->dividerRow = divR.a.y;

    // The vertical scrollbar belongs to the tree only: start it below the filter
    // area so it doesn't run up alongside the filter rows.
    {
        TRect sr = vsb->getBounds();
        sr.a.y = treeR.a.y;
        vsb->setBounds(sr);
        vsb->growMode = gfGrowLoX | gfGrowHiX | gfGrowHiY;
    }

    tree = new DocumentTreeView(treeR, hsb, vsb, nullptr);
    tree->growMode = gfGrowHiX | gfGrowHiY;
    tree->win = this;
    insert(tree);
    setCurrent(tree, normalSelect); // the tree is the default focus, not the box
}

void DocumentTreeWindow::focusFilter() noexcept
{
    if (filterBox)
        filterBox->select(); // make it the current view; cursor lands in it
}

void DocumentTreeWindow::applyFilterFromBox() noexcept
{
    if (filterBox && tree)
        tree->setFilter(filterBox->data);
}

void DocumentTreeWindow::clearFilter() noexcept
{
    if (filterBox)
    {
        char empty[256] = {};
        filterBox->setData(empty); // clears the text and resets the cursor
        filterBox->drawView();
    }
    if (tree)
        tree->setFilter("");
}

void DocumentTreeWindow::clearFilterAndFocusTree() noexcept
{
    clearFilter();
    if (tree)
        tree->select();
}

DocumentTreeWindow::~DocumentTreeWindow()
{
    if (ptr)
        *ptr = nullptr;
}

TColorAttr DocumentTreeWindow::mapColor(uchar index) noexcept
{
    // Same resolution as BasicEditorWindow: map the window palette indices onto
    // the shared chrome scheme so the frame and scrollbars match the editors.
    if (index > 0 && index - 1 < turbo::WindowPaletteItemCount)
        return turbo::windowSchemeActive[index - 1];
    return errorAttr;
}

void DocumentTreeWindow::setBranchInfo(std::string_view info) noexcept
{
    if (info.empty())
        titleBuf = baseTitle;
    else
        titleBuf = baseTitle + " - " + std::string(info);
    frame->drawView();
}

const char *DocumentTreeWindow::getTitle(short)
{
    return titleBuf.empty() ? baseTitle.c_str() : titleBuf.c_str();
}

void DocumentTreeWindow::handleEvent(TEvent &ev)
{
    if (ev.what == evMouseDown)
    {
        TPoint m = makeLocal(ev.mouse.where);
        // The left border (between the corners) is a horizontal resize handle:
        // drag it left/right to widen/narrow the docked tree. onResizeTo re-lays
        // out the editors beside it on each step. The close box sits on the top
        // border (row 0), so starting at row >= 1 never steals it.
        if (m.x == 0 && m.y >= 1 && m.y < size.y - 1 && onResizeTo)
        {
            do {
                onResizeTo(ev.mouse.where.x);
            } while (mouseEvent(ev, evMouseMove | evMouseAuto));
            clearEvent(ev);
            return;
        }
    }
    TWindow::handleEvent(ev);
}

void DocumentTreeWindow::setState(ushort aState, Boolean enable)
{
    TWindow::setState(aState, enable);
    // The tree rows dim when the window is inactive; repaint them on the change
    // (the frame already repaints itself).
    if ((aState & sfActive) && tree)
        tree->drawView();
}

void DocumentTreeWindow::close()
{
    message(TProgram::application, evCommand, cmToggleTree, 0);
}
