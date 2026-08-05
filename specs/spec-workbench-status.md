# Spec Workbench — status & next steps (handoff)

Branch: `spec_management` (do **not** merge to main yet). Pushed to
`origin` (aestubbs/turboIDE). Paused 2026-08-05 to work on something else.

This session reworked the spec-agent UX, then hit a design decision to **pivot
away from the in-IDE structured chat** (see "Decision: Option A" below). The
scroll-wheel and UX fixes are done and verified; the pivot itself is **not
started**.

---

## Done & verified (in the root `./turboIDE`)

The decisions are recorded in `specs/spec-agent-integration.md` (ledger D25–D28,
milestone M5); this is the operational summary.

- **Auto-dock on interactive spec open (D25).** Opening a spec yourself (file
  dialog, tree, Goto Anything, New Spec) docks the agent conversation into the
  spec's editor window in Discuss mode — no `cmSpecWorkbench` command, no launch
  confirmation. Gated on a `userInitiated` flag threaded through
  `openOrFocus`/`fileOpenOrNew`/`addEditor`; bulk/programmatic opens (session
  restore, CLI args, DAP, Spec Manager) stay plain editors. Navigating to an
  already-open (e.g. restored) spec docks it too.
- **Retired menu items.** View ▸ *Spec Workbench* (`cmSpecWorkbench`) and
  *Agent Conversation* (`cmAgentChat`) are gone, along with the standalone
  `AgentChatWindow` scaffold. `Implement Spec…` keeps its readiness gate.
- **`TURBO_NO_AUTO_SPEC_AGENT=1` (D28)** suppresses the auto-dock (escape hatch
  for CI/tests / not spawning an agent on every open).
- **Frame-connected divider (D26).** The Workbench divider runs full height and
  ties into the window frame with box-drawing junctions (`╤`/`╧` active,
  `┬`/`┴` passive; the top cap is skipped when it would land on the title).
  Editor scrollbar sits left of the divider, chat scrollbar on the right frame.
- **Mouse wheel acts on the window/control UNDER THE CURSOR (D27).** Both
  cross-window (tree vs editor) and intra-window (two panes). tvision fork
  `TGroup::handleEvent` routes the wheel positionally; `TScroller` (file tree),
  `TListViewer` (output pane, Spec Manager, dialog lists) and `EditorView` now
  self-scroll (the terminal already did). See
  `memory/turbo-mouse-wheel-under-cursor.md` for the `sfVisible` gotcha.

**Tests (all green):** `spec_workbench_test` 19/19, `spec_identity_test` 53/53,
`spec_scroll_test` 4/4, `wheel_test` 5/5 (new, cross-window), 98 unit tests.

**Build/run reminder:** the binary the user runs is the **root-tree**
`./turboIDE` (`cmake --build .`; `OUTPUT_NAME turboIDE`, Release,
`TURBO_MINIMIZE_SIZE` strips symbols). `build-ide/` is a *separate* dev tree —
building it does **not** update `./turboIDE`. (This bit us badly: a whole round
of "nothing works" was just a stale root binary.)

---

## Decision: Option A — retire the in-IDE structured chat (NOT started)

The docked structured chat (`AgentConversationView` + the stream-json
`SpecAgentSession`) has structural problems that a rebuild won't fix:

- the composer is a single line;
- **tool-permission approvals can't be granted** — it drives `claude -p
  --output-format stream-json`, a one-way stream with no channel to answer a
  permission prompt, so the agent can't actually act;
- a list-box-with-selector is the wrong primitive for a chat log;
- the opening brief is dumped visibly into the transcript.

**Decision (user):** drop the docked chat. Keep the **Spec Manager** (lifecycle
tracking — the genuinely IDE-native, valuable piece). Drive spec development
from the real **Alt-0 Claude Code terminal** (`toggleAgent` → `TerminalWindow`,
which already gives multi-line input, permission prompts and proper scrollback)
guided by a **spec-development skill**, editing specs through the existing
**MCP bridge** (`turboIDE mcp` — `ask_user`, `file_text`, `insert_text`, `save`,
section jump).

---

## Next steps (ordered)

1. **Verify the scroll wheel in real Ghostty use** (tree, editor, output pane) —
   PTY-verified but eyeball it before building more.
2. **Option A teardown + skill.**
   - Remove the docked pane path: `AgentConversationView`, `ComposerInputLine`,
     `SpecAgentPane`/`SpecAgentSession`, `specworkbench.cc`, the `EditorWindow`
     agent-pane members/methods, `cmFocusAgentPane`/`cmJumpToolTarget`, the
     idle `pumpAgentPane()` loop, and the auto-dock call in `addEditor`
     (`ensureSpecWorkbench`/`openSpecWorkbench`). Keep the Spec Manager, the MCP
     bridge, and the divider/junction frame code is then unused — remove it too.
   - Keep the D26/D27 scroll and frame work? The **wheel fix stays** (general
     improvement). The **divider/junction** code becomes dead once the pane is
     gone — remove `agentDividerColumn()` + `EditorFrame` junctions +
     `layoutAgentPane`.
   - Add a spec-development skill (methodology: readiness rubric, write-back
     contract, `ask_user` for enumerable decisions) that the terminal agent
     loads; point the Spec Manager's Discuss/Draft/Implement actions at the
     terminal agent (or just document the workflow) instead of the docked pane.
   - Update `specs/spec-agent-integration.md` with a D29 recording the reversal
     of the docked-chat approach (supersedes D1/D12/D25's docked pane).
3. **File-tree right-click "Delete…" with confirmation.** The tree already has a
   right-click context menu (`DocumentTreeView::showContextMenu`, `doctree.cc`);
   add a Delete entry that confirms (`messageBox mfWarning|mfYesButton|mfNoButton`)
   then removes the file/dir and refreshes the tree.
4. Polish made moot by Option A (skip if pivoting): multi-line composer, hide
   the brief, a View-menu "Spec" submenu.

---

## Where things live

- Decisions/FRs: `specs/spec-agent-integration.md` (D25–D28, M5), annotated
  `specs/spec-workbench.md` (D25).
- Auto-dock + menu + confirmation: `source/turbo/app.cc` (`ensureSpecWorkbench`,
  `openSpecWorkbench`, `addEditor`, `openOrFocus`, `specAutoAgentEnabled`),
  `source/turbo/app.h`, `cmds.h`, `commandpalette.cc`, `gotoanything.cc`.
- Docked pane + divider: `source/turbo/specworkbench.{cc,h}`,
  `source/turbo/editwindow.{cc,h}` (`EditorFrame::draw` junctions,
  `agentDividerColumn`).
- Chat view (to be removed in A): `source/turbo/agentconversationview.{cc,h}`,
  `source/turbo/specagentsession.{cc,h}`.
- Wheel fix: `deps/tvision` submodule (`tgroup.cpp`, `tscrolle.cpp`,
  `tlstview.cpp` — committed + pushed to aestubbs/tvision `master`),
  `source/turbo-core/editview.cc`. Tests `test/pty/wheel_test.py`,
  `test/pty/spec_scroll_test.py`.
