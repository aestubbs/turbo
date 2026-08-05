---
title: Spec Agent Integration — one window, one conversation
status: implemented
domain: ide-feature
created: 2026-07-20
updated: 2026-07-22
depends: spec-workbench
---

# Background

The Spec Workbench works. `cmSpecWorkbench` puts a spec on the left and a
coding agent on the right, the agent edits the spec, the filewatcher reloads
it, and `ask_user` raises native dialogs over the MCP bridge. This spec is
not a rewrite of that; it is the "revisit" clause D9 wrote into it.

What exists today, precisely:

- **The Workbench is not a window.** `TurboApp::specWorkbench()`
  (`app.cc:3805`) calls `locate()` on two independent top-level windows —
  the spec's `EditorWindow` and the app-global agent terminal — to place
  them side by side. There is no container (D24 recorded this as
  deliberate).
- **There is exactly one agent window in the whole application.**
  `TurboApp::agentWin` is a single `TerminalWindow *` (`app.h:112`), shared
  with the Alt-0 general agent. Opening a Workbench on a second spec
  re-points the same window, so a second spec cannot hold its own
  conversation.
- **The agent is a terminal.** The right pane is a full VT emulator —
  libvterm, a 10,000-line scrollback, mouse forwarding, OSC titles
  (`terminal.cc`, 1090 lines). turbo receives a character grid, so it
  cannot tell a tool call from prose, and cannot attach behaviour to
  anything the agent says.
- **The launch prompt is malformed.** `shellQuoteArg` (`specagent.cc:218`)
  wraps the prompt in POSIX single quotes, but `splitCommand`
  (`terminal.cc:181`) splits on whitespace with no quote awareness and
  `PtyProcess::start` calls `execvp` directly — there is no shell in the
  path. Every Discuss/Draft/Implement launch delivers the prompt as ~13
  separate argv entries with stray quote characters. The PTY tests do not
  catch this: they assert the brief file was written and the window opened,
  never what the child process received.

Three consequences follow, and they are the whole motivation for this spec:
the panes drift apart because nothing owns them jointly; only one spec can
be under discussion at a time; and the richest thing turbo can do with the
agent's output is print it.

# Objective

**One window per open spec.** A single window whose left pane is the spec in
a normal editor and whose right pane is a conversational agent view. Open
three specs, get three windows, each with its own live conversation.

Everything else follows from making the agent's output structured rather
than pixels: turbo can render a tool call as a tool call, offer the IDE's
own affordances at the right moments, and lean harder on `ask_user` dialogs
instead of questions buried in scrollback.

The agent stays Claude Code. This spec introduces no API key, no credential
storage, and no billing change.

# Non-Goals

- **Not replacing Claude Code.** The engine stays a commodity agent CLI
  (D2/D5 stand). Calling the Anthropic API from inside turbo is a separate,
  later spec — see D6 below for why it is separated and what it would cost.
- **Not removing the terminal.** `TerminalWindow` keeps its job for shells
  and for the Alt-0 general agent. Only the *spec* agent stops being a
  terminal.
- **Not changing the spec model, the readiness rubric, the implementation
  gate, or the Spec Manager table.** `specmodel.cc`, `specagent.cc`'s domain
  packs, and `askdialog.cc` are all reused unchanged.
- **Not multi-agent.** One conversation per spec window.

# Functional Requirements

## The Workbench window

- **FR1** — Opening a spec interactively (file dialog, tree, Goto Anything,
  New Spec) docks the agent conversation into that spec's editor window: a
  single `TWindow` containing the spec editor (left) and the conversation
  view (right), separated by a draggable splitter. Moving, resizing, zooming,
  or closing the window acts on both panes together. It is an ordinary window,
  so several may be open at once (D10 stands). Revised by **D25 (2026-07-23):**
  the dock is automatic on interactive open, replacing the `cmSpecWorkbench`
  command; bulk/programmatic opens (session restore, CLI args, DAP) stay plain
  editors.
- **FR2** — One window per spec. Invoking the Workbench on a spec that
  already has one focuses the existing window rather than opening a second.
- **FR3** — Each window owns its own agent session. Two open Workbench
  windows hold two independent conversations, neither of which is the
  Alt-0 general agent.
- **FR4** — The left pane is a full editor: editing, save, undo, syntax
  highlighting, the purple spec surface, and the FR6 section strip all
  behave exactly as they do in a standalone spec window. This is the
  existing `EditorView` plus its left margin and scrollbars, reparented
  into the container — not a reimplementation.
- **FR5** — Tab moves focus between panes. Closing the window ends the
  agent session cleanly.

## Structured agent transport

- **FR6** — The spec agent runs headless with a structured event stream
  rather than a pty: one long-lived child process driven with
  `--input-format stream-json --output-format stream-json`, user turns
  written to its stdin as JSON, agent events read from stdout as
  newline-delimited JSON. This reuses the transport shape turbo already
  implements three times (`lsp::Client`, `dap::Client`, `CommandRunner`):
  a blocking reader thread, a mutex-guarded queue, `TEventQueue::wakeUp()`,
  and dispatch on the main thread from `pump()`.
- **FR7** — Session continuity uses the agent's own session store. turbo
  captures the session id from the stream's init event and passes
  `--resume` when reattaching, so closing and reopening a Workbench
  resumes the conversation. This makes D25's "delegate transcripts to the
  agent CLI" concrete.
- **FR8** — The prompt is delivered as a structured message, not an argv
  fragment. This retires `shellQuoteArg` and the brief-file indirection
  that only existed to dodge shell quoting, and fixes the malformed-argv
  bug described in Background.
- **FR9** — Reversed by **D25 (2026-07-23):** agent launches on interactive
  spec open are *not* confirmed. The command line comes only from agent config
  the user set up, never from spec content, so there is nothing for a per-open
  dialog to vet; a modal on every spec open was friction on the primary path.
  `TURBO_NO_AUTO_SPEC_AGENT` (D28) is the opt-out for anyone who wants to open
  specs without launching an agent.

## Conversation view

- **FR10** — The right pane renders typed events, not a character grid:
  user turns, assistant prose, tool calls, tool results, and errors are
  each their own item with their own styling. The view scrolls, selects,
  and copies.
- **FR11** — Tool calls are legible: the pane shows which tool ran with
  which target, rather than whatever the agent chose to print. A tool call
  naming a spec section is clickable and moves the left pane's cursor to
  that section.
- **FR12** — A message composer at the foot of the right pane sends a turn
  to the agent. Enter sends; the pane shows the turn as pending until the
  agent acknowledges it.
- **FR13** — `ask_user` questions continue to raise native wizard dialogs
  over MCP exactly as they do now (FR11/FR12 of `spec-workbench`), and the
  domain pack is amended to prefer them for any decision with enumerable
  options. Structured transport does not replace `ask_user`; it makes the
  surrounding conversation match its quality.

## What is unaffected

- **FR14** — The Alt-0 general agent, all terminal windows, `cmNewSpec`,
  the Spec Manager table, `Mark reviewed…`, the implementation gate
  (FR20 of `spec-workbench`), the doctree tint, and the purple identity
  are untouched by this spec. The Manager's **Discuss** action opens a
  Workbench window instead of tiling two; **Draft** and **Implement** keep
  their current terminal launch until M4.

# UX Considerations

- **The window is the unit.** Today the two panes are separate windows that
  drift apart the moment you move either one, and Alt-Tab cycles them
  independently. One window with two panes removes an entire category of
  fiddling — and makes "three specs open, three conversations" a coherent
  thing to look at rather than an impossible one.
- **The document is still primary.** The split defaults to a wider left
  pane. The conversation is scaffolding for the artifact (unchanged from
  `spec-workbench`'s UX section).
- **Structure earns its keep only if it is used.** A conversation view that
  merely reprints what the terminal printed is not worth the code. The test
  is FR11: tool calls that are clickable, and questions that arrive as
  dialogs rather than as text asking you to type a number.
- **Keyboard-first.** Tab cycles panes, the composer is a normal input, and
  the wizard is fully keyboard operable. No new hotkey: Workbench windows
  are ordinary windows (D10).

# Security Considerations

- **No credential is introduced.** The agent authenticates itself, as it
  does today. turbo has never held a secret of any kind — `gitclient.cc`
  deliberately sets `GIT_TERMINAL_PROMPT=0` and defers to the user's own
  credential helper — and this spec preserves that posture exactly. See
  D5 for what changes if that is ever revisited.
- **Prompt injection via spec content** is unchanged from `spec-workbench`:
  the brief frames the spec as data, `ask_user` dialogs carry agent
  attribution so they cannot be mistaken for IDE chrome, and destructive
  actions stay behind the agent's own permission model.
- **Structured output is parsed defensively.** Agent stdout is untrusted
  input. Parsing follows the existing convention —
  `Json::parse(body, nullptr, false)` then `.is_discarded()` — never
  throwing, never assuming a field is present, and never rendering
  agent-supplied text as IDE chrome.
- **The MCP socket** keeps its current trust model (0600, owning user
  only); nothing in this spec widens it.

# Test Strategy

- **Unit tests:** stream-json event parsing (well-formed, truncated,
  interleaved, and garbage lines); session-id capture and `--resume`
  argument assembly; conversation-model append/scroll logic; the
  argv-assembly fix (assert the prompt arrives as exactly one argument).
- **PTY runtime tests** (project conventions: real pty, `TIOCSWINSZ`,
  discrete keys, non-symlinked project dir): opening the Workbench yields
  **one** window containing both panes; two specs yield two independent
  windows; editing in the left pane still saves; Tab moves focus; closing
  the window ends the child process.
- **Scripted agent test, no LLM:** a fake agent binary that speaks the
  stream-json protocol — emits text, emits a tool call, calls `ask_user`
  over the real MCP bridge, writes a spec section — asserting the
  conversation view renders each event type and the write-back reloads
  into the left pane. This extends the existing M5 agent-loop test rather
  than replacing it.
- **Regression:** the existing 44 `spec_identity_test.py` checks must keep
  passing, in particular the purple identity in both colour modes and the
  gated Implement handoff.
- **Build hygiene:** new `.cc` files need `cmake .` re-run (glob); watch
  unity-batch include shifts; verify built=2 errs=0 with LSP on and off
  before commit.

# References

- `specs/spec-workbench.md` — the parent spec; D9, D10, D24, D25 are the
  decisions this one revisits.
- `source/turbo/app.cc:3805` (`specWorkbench`), `:3700` (`launchSpecAgent`)
  — the tiling and launch paths being replaced.
- `source/turbo/terminal.cc`, `terminal.h` — the pty/VT agent pane, and
  `splitCommand` (`:181`), half of the argv bug.
- `source/turbo/specagent.cc` — domain packs, brief assembly, and
  `shellQuoteArg` (`:218`), the other half.
- `include/turbo/editor.h:210` (`EditorView`), `source/turbo-core/basicwindow.cc:25`
  — proof the editor is a plain view and can be reparented into a container.
- `include/turbo/lsp/client.h`, `include/turbo/dap/client.h`,
  `source/turbo/commandrunner.h` — the three existing reader-thread +
  wake/pump transports this reuses.
- `source/turbo/mcpserver.cc`, `source/turbo/askdialog.cc` — the tool
  surface and question wizard, both reused unchanged.

# Decisions

- **D1 (2026-07-20):** One window, not two placed windows. This supersedes
  D24's "no new container window": that call was right for M4 (it bought a
  working split cheaply) but it is what makes the panes drift and caps the
  IDE at one conversation. `SpecWorkbenchWindow` becomes a real container.
- **D2 (2026-07-20):** Per-window agent session, replacing the single
  app-global `TurboApp::agentWin` for spec work. The Alt-0 general agent
  keeps the global slot and stays a terminal; the two no longer share one
  window.
- **D3 (2026-07-20):** The engine stays Claude Code, driven headlessly.
  D2/D5 of `spec-workbench` are unchanged in substance — turbo still drives
  a commodity agent CLI, and the value is still the management layer around
  it. What changes is the *interface* to that CLI: structured JSON over
  stdio instead of a terminal.
- **D4 (2026-07-20):** The transport is the existing one. A long-lived
  child process with a blocking reader thread posting to the UI thread via
  `wakeUp()`/`pump()` is how LSP, DAP, and `CommandRunner` already work.
  No new dependency, no event loop, no async IO abstraction.
- **D5 (2026-07-20):** No credential, deliberately. Because the agent
  authenticates itself, this spec adds no key, no keychain, and no billing
  change — which matters given the IDE is routinely used over SSH on a
  remote machine, where OS keychains are the *worst* option (macOS Keychain
  needs an unlocked session; libsecret needs a session bus; both fail on a
  plain SSH login). If turbo ever calls the API directly (D6), the
  SSH-compatible answers are a device-code browser flow — turbo shows a
  short code and a URL, you authorise on any browser anywhere, turbo stores
  the resulting refresh token — or a one-time paste into a dialog stored
  `chmod 600` under `~/.turbo/`. An environment variable is rejected as the
  primary mechanism on ease-of-use grounds, and `~/.turborc` is rejected
  outright: it is plaintext, parsed by hand-rolled `sscanf` into a fixed
  1024-byte buffer, and never permission-restricted.
- **D6 (2026-07-20):** Calling the Anthropic API in-process is deferred to
  its own spec, not folded in here. It would buy real things — the FR14
  write-back contract enforced in code rather than requested in a prompt,
  and deletion of the MCP socket and bridge (~900 lines that exist only so
  an out-of-process CLI can reach back into the IDE). It costs a TLS
  dependency across four CI configurations, two of them MSVC with
  `TURBO_USE_STATIC_RTL=ON`, in a project that today links nothing beyond
  libncurses, CoreServices, libc++ and libSystem and has no package manager
  in its build. Recorded now so the roadmap remembers: if it happens,
  vendor mbedTLS as a submodule (matching the existing 16-submodule,
  compiled-from-source model; CMake-native; static-CRT friendly; small
  enough for the size budget `TURBO_MINIMIZE_SIZE` guards) rather than
  OpenSSL, which fits none of those constraints. Crucially, the work in
  this spec is not thrown away by that move: the window, the conversation
  view, the event model, and the tool rendering are all transport-agnostic.
  Going in-process later swaps what feeds them.
- **D7 (2026-07-20):** The malformed-argv bug is fixed as part of M1 rather
  than patched separately, because FR8 retires the quoting path entirely.
  A unit test asserting the prompt arrives as exactly one argument is the
  regression guard.

- **D8 (2026-07-20):** M1 implementation notes. (a) The wire format was
  verified empirically against Claude Code 2.1.x rather than assumed, and
  two facts drove the design: the child **stays alive across turns**, and
  each turn is its own `system/init` → `assistant` → `result` cycle, so
  `result` marks end-of-*turn*, not end-of-session. That is what makes one
  long-lived session per window possible. The init event also reports
  `apiKeySource: "none"`, confirming D5 — the agent authenticates itself and
  turbo holds no credential. (b) `--verbose` is *required* alongside
  `--output-format stream-json`, not cosmetic. (c) Split across tiers on the
  `specmodel` precedent: the pure protocol (argv assembly, turn encoding,
  event parsing) is `turbo-core/specagentproto.cc` so it can be unit tested
  — the test target links only `turbo-core` — while the threaded transport
  is `source/turbo/specagentsession.cc`. (d) Parsing *appends* events rather
  than returning one, because a single assistant message routinely carries
  prose plus several `tool_use` blocks. (e) The session exposes an `onWake`
  hook instead of calling `TEventQueue::wakeUp()` directly the way
  `CommandRunner` does, keeping the wake policy in the app tier and avoiding
  the latency gap `lsp::Client` has from lacking one. (f) `specagentsession`
  compiles but is not yet referenced — wiring it in is M2/M3, per this
  milestone's "no UI change".
- **D9 (2026-07-20):** Implementation discovery (per FR14 of
  `spec-workbench`), non-blocking: tvision's vendored test target
  (`deps/tvision/test/CMakeLists.txt`) resolves gtest with `find_library`
  but never `find_path` for its headers, so `tvision-test` cannot compile on
  macOS/Homebrew whenever `TURBO_BUILD_TESTS` is on in the root build. D21(e)
  of `spec-workbench` fixed exactly this for turbo's own tests and did not
  reach the submodule. Worked around rather than fixed: the root build keeps
  `TURBO_BUILD_TESTS=OFF` and `build-tests/` is the test build directory.

- **D10 (2026-07-20):** M2 implementation notes. (a) Same tier split as M1:
  the transcript is a pure model (`turbo-core/agentconversation.cc` —
  event-to-item mapping, wrapping, layout caching) with the tvision face in
  `source/turbo/agentconversationview.cc`, so the wrapping is unit tested.
  (b) The view is a `TListViewer` over *flattened, pre-wrapped* rows, exactly
  as `OutputView` is over output lines — that buys scrolling, the scrollbar,
  focus, and mouse handling for free rather than hand-rolling a scroller.
  Rows carry an index back to the item that produced them, which is what M4's
  clickable tool calls will use. (c) Wrapping uses `TText::next`, the same
  primitive tvision draws with, so it agrees with rendering on wide glyphs
  and never splits a UTF-8 sequence; leading indentation is preserved and
  re-applied to continuation rows, because tool results rely on it to read as
  subordinate to their call. (d) Item markers are ASCII (`>`, `*`, `!`) with
  colour and indentation carrying the structure: glyph coverage cannot be
  detected at runtime, so an exotic marker risks rendering as tofu.
  (e) Tool results are clipped for display (6 lines / 400 chars) — they are
  routinely whole files, and an unclipped one buries the conversation; the
  agent still received all of it. (f) `ComposerInputLine` is a third
  Enter-handling variant: raw `TInputLine` eats Enter, `FieldInputLine` maps
  it to `cmOK` (right in a dialog, wrong here), so the composer maps it to
  send. (g) Verified against the real `claude` binary, not only the scripted
  fake: a live turn round-tripped into the pane with no raw protocol reaching
  the screen.
- **D11 (2026-07-20):** Test-harness discovery, recorded because it will bite
  every future PTY assertion on rendered text: tvision repaints
  *differentially*, so cells already holding the right glyph are never
  rewritten. A run of spaces that was already blank produces no output at
  all, and once the cursor-positioning escapes are stripped the glyphs that
  *were* written land adjacent — "ECHOED hello" reads as "ECHOEDhello" even
  though the screen is correct. PTY tests must compare whitespace-free
  (`squash()` in `agent_chat_test.py`) rather than asserting exact spacing.
  This cost a false failure before it was understood.

- **D12 (2026-07-20):** M3 took the opposite route to the one this spec
  proposed, and the result is simpler. Rather than build a container window
  and reparent the editor into it, the conversation **docks into the spec's
  own `EditorWindow`** — the same move `setSpecSectionsMode` already makes
  for the section strip, but on the horizontal axis. The window is therefore
  one window *by construction*, and the editor keeps every behaviour it
  already has (save, undo, LSP, the purple surface, the filewatcher reload,
  the section strip) because it is still the same window — none of which
  survives a reparent for free. FR1's "container" wording is satisfied in
  substance; D1's supersession of D24 stands.
- **D13 (2026-07-20):** Crossing panes is Alt-Right/Alt-Left, not Tab.
  `EditorView::handleEvent` calls `clearEvent()` on **every** keystroke, so
  no key escapes the document once it has focus — Scintilla legitimately owns
  Tab for indentation. The handler therefore lives in
  `EditorWindow::handleEvent`, which sees events before the focused subview;
  a status-line binding cannot work here and an earlier attempt at one raced
  the window handler (it routed to a *toggle*, so the first press could move
  focus the wrong way). Focus after docking is also set explicitly to the
  document rather than left to `TGroup::resetCurrent`, since inserting
  selectable views makes the group re-pick. Tab still returns from the
  composer, where nothing else claims it.
- **D14 (2026-07-20):** The Discuss brief now travels as the opening
  structured turn, so `writeSpecBrief`/`shellQuoteArg` are off the Workbench
  path entirely (FR8). `launchSpecAgent` still serves Draft and Implement
  until M4 moves them across; the brief-file mechanism therefore still
  exists, but the Workbench no longer touches it.
- **D15 (2026-07-20):** Spec identity extended to every window that shows
  spec material. The palette moved out of `editwindow.cc` into a shared
  `speccolors.{h,cc}` (the `fieldinput.h` precedent from D21a — a
  function-local static duplicated across unity batches is the landmine that
  removed), and the Spec Manager now resolves its chrome *and* its rows
  through it: violet ground, lavender frame, gold header accent. The header
  accent had to change — it was the spec purple `0x9D7CD8`, which vanishes
  once the ground itself is purple; gold matches the frame icons and ties
  into the classic blue/gold palette. Classic 16-colour mode gets magenta
  chrome with white/yellow accents.
- **D16 (2026-07-20):** Test-harness discovery, non-blocking but worth
  recording: `/bin/cat` is no longer usable as a stand-in agent. With stdout
  on a pipe it block-buffers at 4096 bytes and the interview brief is ~4044,
  so it never flushes and the pane stays empty — which reads exactly like a
  product bug. The suites now use a scripted fake that speaks stream-json
  and logs what it receives, which is also stronger: the brief's content is
  asserted *as the agent receives it* rather than by reading a file turbo
  wrote.

- **D17 (2026-07-20):** M4 notes. (a) Tool-call targeting is a pure function
  in core (`convToolTargetLine`): an explicit line/offset field in the call's
  input wins, else the longest canonical section title named anywhere in it,
  else no target. Parsed off the raw JSON text rather than a DOM, because the
  field sits at different depths under different names and this layer must
  not care which tool produced it. (b) The jump reads the *live buffer*, not
  the file on disk, so it lands correctly with unsaved edits.
- **D18 (2026-07-20):** FR11's "clickable" gained a keyboard twin, `Alt-J`
  (Jump to Agent's Last Edit), and the reason is worth recording: making the
  transcript keyboard-reachable proved unreliable. `TWindow::handleEvent`
  claims Tab for `focusNext` before any subview sees it, and `select()` on
  the transcript view did not reliably take focus. Rather than fight the
  toolkit, the jump became a command that works from either pane and targets
  the most recent tool call; double-click still activates a specific one.
  Alt-Right/Alt-Left cycle document → composer → transcript → document.
- **D19 (2026-07-20):** `SCI_GOTOLINE` moves the caret without scrolling, and
  `SCI_SCROLLCARET` alone scrolls *minimally* — which lands a section heading
  on the bottom edge with everything the agent wrote still below the fold.
  The jump therefore also sets the first visible line, so the target lands
  near the top with its content beneath it. Found because the PTY assertion
  failed while the feature "worked".
- **D20 (2026-07-20):** All three modes now run in the Workbench:
  `openSpecWorkbench(path, mode)` is the single path, and Draft/Implement no
  longer spawn a terminal. The implementation gate (FR20 of `spec-workbench`)
  is unchanged and still evaluated before Implement opens anything.
  `launchSpecAgent`/`writeSpecBrief`/`shellQuoteArg` are now unreferenced by
  the spec flows; they are left in place rather than deleted in the same
  change, so the diff stays reviewable.
- **D21 (2026-07-20):** Near-miss worth recording: refactoring
  `specWorkbench()` by replacing a source range silently swallowed
  `TurboApp::toggleAgent()`, breaking the Alt-0 agent window. The linker
  caught it, but a range-based edit that spans a function boundary is a
  landmine — the restored copy came from `git show HEAD:` rather than being
  retyped.

- **D22 (2026-07-22):** FR7 (session resume) was specced and documented but
  never actually wired -- surfaced by a later spec-vs-code review (the FR14
  discovery loop). M1 built the mechanism (`specAgentArgv` appends `--resume`,
  `SpecAgentSession::start` takes a resume id, and the id is captured from the
  stream's init event) and even unit-tested the argv, but nothing ever stored
  or replayed the id, so `sessionId()` had zero readers and every reopen
  started a fresh conversation. Closed with a per-spec-path session-id map on
  `TurboApp` (`rememberSpecSession`/`recallSpecSession`, on the
  `EditorWindowParent` seam), captured live on `SessionStart`;
  `openSpecWorkbench` passes the remembered id and **skips re-sending the
  brief** on a resume, because `--resume` restores the conversation's context
  and re-briefing would be noise. A PTY test asserts the close→reopen
  round-trip delivers `--resume <captured id>`.

- **D23 (2026-07-22):** FR1/M3's "draggable splitter" was drawn but inert:
  `SpecPaneDivider` had only `draw()`, and the split was a fixed 48 columns.
  Also closed from the same review. The divider now handles the mouse drag
  with the standard `mouseEvent()` loop and re-splits through
  `layoutAgentPane`, which already clamps so neither pane can be squeezed out.
  `TGroup` routes the positional event to the divider via `firstThat(hasMouse)`
  even though it is not selectable, so no focus change was needed. Verified by
  an SGR mouse-drag PTY test (turbo enables `?1000h`/`?1002h`/`?1006h`;
  asserting the move needs a reconstructed screen grid, since `squash()` loses
  columns).

- **D24 (2026-07-22):** The dead `launchSpecAgent` / `writeSpecBrief` /
  `shellQuoteArg` that D20 deliberately left in place "so the diff stays
  reviewable" are now deleted, per that decision's intent, once the structured
  path (D14/D20) had fully replaced them and nothing else referenced them. No
  behaviour change -- the brief travels only as the opening structured turn
  (FR8) -- and `<filesystem>` went with `writeSpecBrief` as its sole user.

- **D25 (2026-07-23):** The Workbench is no longer a command. Opening a spec
  interactively auto-docks the agent (Discuss), and the launch confirmation is
  gone. This supersedes D24's "no new container window" chain's assumption of a
  manual `cmSpecWorkbench` trigger, FR4/FR17/D24 of `spec-workbench` ("separate
  command"), and FR9 above / `spec-workbench` D25 ("user-confirmed launch").
  Rationale: the docked pane *is* the spec-editing experience, so a menu command
  plus a modal were friction on every open; the command line is trusted agent
  config, never spec content, so the confirmation guarded nothing. Both retired
  menu items -- `cmSpecWorkbench` ("Spec Workbench") and the older standalone
  `cmAgentChat` ("Agent Conversation", the pre-dock `AgentChatWindow` scaffold)
  -- and the confirmation are removed; `Implement` keeps its readiness gate but
  loses the modal. "Interactive" is a `userInitiated` flag threaded through
  `openOrFocus`/`fileOpenOrNew`/`addEditor` and set only at the user-open entry
  points, so restore/CLI/DAP/Manager opens stay plain editors. The found-and-
  focused branch of `openOrFocus` docks too, so navigating to an already-open
  (e.g. session-restored) spec starts its conversation. Verified by the updated
  `spec_workbench_test.py` (19 checks) and `spec_identity_test.py`.

- **D26 (2026-07-23):** The split reads as one surface. The divider runs the
  full inner height and ties into the window frame with box-drawing junctions
  drawn by `EditorFrame` after `TFrame::draw` (the `OutputFrame::drawTabs`
  pattern): `╤`/`╧` under an active (double) border, `┬`/`┴` under a passive
  (single) one, coloured to match the frame. The bottom junction is always
  drawn; the top is skipped when the divider column falls within the centered
  title, so the filename is never carved into -- so in a wide window both
  connect, and in a narrow one only the bottom does. The editor's vertical
  scrollbar sits just left of the divider and the conversation's on the right
  window frame (this pins the placement `spec-workbench` D29 left open).

- **D27 (2026-07-23):** The mouse wheel acts on the view/window under the
  cursor, not the focused one. Upstream Turbo Vision excludes `evMouseWheel`
  from `positionalEvents`, so `TGroup::handleEvent` broadcast it front-to-back
  and the focused window's scrollbar ate it. The fork now special-cases the
  wheel in `TGroup::handleEvent`: route it to `firstThat(hasMouse)` and fall
  back to the in-group broadcast only if that view doesn't consume it (so a
  single-scrollbar window -- tree, output, help -- still works). Because the
  spec window holds two vertical scrollbars in one group, the editor and the
  conversation views each own the wheel (`EditorView` forwards to its own bars
  via `scrollBarEvent`; `AgentConversationView` moves its own `topItem`), so the
  hovered pane scrolls and the other stays put -- no divider-column geometry.
  The mask is left untouched so the editor's button-held drag loops are
  unaffected. Verified by `spec_scroll_test.py` under SGR wheel events.

- **D28 (2026-07-23):** `TURBO_NO_AUTO_SPEC_AGENT=1` suppresses the D25
  auto-dock, leaving an interactively-opened spec a plain editor. An escape
  hatch for CI/tests (the surface-identity PTY sessions assert on the plain
  purple editor) and for users who would rather not spawn an agent on every
  spec open. Off by default; the explicit actions (Manager Discuss, Implement)
  ignore it.

Open questions: none at present. Implementation may surface new ones (FR14
of `spec-workbench`).

# Implementation Plan

- [x] **M1 — Structured transport.** `SpecAgentSession`: long-lived child
      driven with stream-json in/out, reader thread, `wakeUp()`/`pump()`
      dispatch, session-id capture and `--resume` reattach, typed event
      model. Retires `shellQuoteArg` and the brief-file indirection;
      fixes the argv bug (D7). No UI change — proven by unit tests plus a
      scripted fake agent.
- [x] **M2 — Conversation view.** `AgentConversationView`: renders the
      typed events (user turn, assistant prose, tool call, tool result,
      error), scrollback, selection, and the message composer (FR10, FR12).
      Still hosted in an ordinary window at this stage.
- [x] **M3 — The Workbench window.** `SpecWorkbenchWindow` container:
      editor pane reparented left, conversation right, draggable splitter,
      Tab between panes, one window per spec, per-window session (FR1–FR5).
      `cmSpecWorkbench` and the Manager's Discuss action switch to it; the
      tiling code is deleted. PTY tests for one-window layout, two
      independent windows, editing, and clean shutdown.
- [x] **M4 — Integration payoff.** Clickable tool calls that move the left
      pane's cursor (FR11); domain-pack amendment pushing `ask_user` for
      enumerable decisions (FR13); Draft and Implement move onto the
      structured session. Docs updated.
- [x] **M5 — Open-to-converse UX (2026-07-23).** Auto-dock on interactive spec
      open with no command and no confirmation (D25/D28); the divider ties into
      the frame with box-drawing junctions and pinned scrollbar sides (D26); the
      mouse wheel acts on the pane/window under the cursor (D27). Retires the
      `cmSpecWorkbench` and `cmAgentChat` menu items and the `AgentChatWindow`
      scaffold. PTY-verified (`spec_workbench_test`, `spec_scroll_test`,
      `spec_identity_test`).

# Summary

The Spec Workbench currently places two independent windows side by side and
shares a single app-global agent terminal between every spec. That caps the
IDE at one conversation, lets the panes drift apart, and reduces the agent to
a character grid turbo cannot reason about. This spec makes the Workbench a
real window: one per spec, spec editor left, conversational agent right, each
with its own session. The agent stays Claude Code and keeps authenticating
itself, so no key, no credential store, and no billing change are
introduced — which matters for remote SSH use, where OS keychains fail. What
changes is the interface: a long-lived headless child speaking newline-delimited
JSON over stdio, using the same reader-thread-and-pump transport turbo already
runs for LSP, DAP, and command execution. Because turbo can then see tool calls
rather than pixels, it can render them as affordances and push the agent toward
native question dialogs — the integration win, rather than a prettier
transcript. Calling the API in-process stays on the roadmap as its own spec,
and nothing built here is wasted if it happens.
