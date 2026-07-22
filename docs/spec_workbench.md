# Spec Workbench — spec-driven development in turbo

The full specification (and the methodology's own dogfood) is
`specs/spec-workbench.md`. This page is the user-facing tour.

## The idea

Specs are the managed artifact; code trends toward regenerable output. Every
spec lives in `specs/` at the project root as plain Markdown with a small
YAML frontmatter (`title`, `status`, `domain`, `created`, `updated`, and
optionally `id`, `depends`, `supersedes`). Lifecycle:

    draft -> ready -> reviewed -> implementing -> implemented   (plus parked)

`ready` is earned from an agent's hard readiness rubric; `reviewed` is the
user's act; implementation is gated (see below).

## Purple means spec

Any file under `specs/` renders as one deep-violet surface — frame, icons,
scrollbars, and text (BIOS magenta in classic 16-colour mode) — with
active/inactive shades; the tree shows `specs/` and its files in the spec
purple. A spec is always recognisably a spec.

## The Spec Manager (Alt-P)

The ALM surface: a table of every spec — Title, Status, Domain, Updated,
Plan progress — newest first, refreshed automatically when anything under
`specs/` changes. The bottom line shows the focused spec's implementation
gate: `Gate: PASS` or `Blocked: …` with each unmet condition.

Row actions:

| Key   | Action                                                        |
|-------|---------------------------------------------------------------|
| Enter | Open the spec in an editor                                    |
| N     | New Spec… (title/domain/goal -> templated `specs/<kebab>.md`) |
| W     | Draft — agent writes the spec autonomously from its goal      |
| D     | Discuss — interview agent + Workbench split                   |
| R     | Mark reviewed… (recorded as a dated Decisions entry)          |
| I     | Implement — gated handoff to the coding agent                 |
| F, /  | Cycle status filter; text filter                              |

## The Workbench

`Spec Workbench` (command palette, or D in the Manager) opens **one window
per spec**: the document on the left, with a live "Sections n/m drafted |
empty: …" strip, and a conversation with the agent on the right. Open three
specs and you get three windows, each with its own independent agent
session. Drag the divider between the two panes to give either one more
room; the document stays primary, so it opens wider.

The conversation is about the document: answers are distilled into
sections, and you can always type into the spec directly — agent writes
reload into the editor automatically.

It is not a terminal. The agent runs headless and turbo renders its output
as typed items, one colour each: your turns, the agent's prose, tool calls
(with the tool and its target), tool results (indented, clipped), and
errors. A message composer sits at the foot of the pane; Enter sends.

**Keys.** `Alt-Right` cycles focus forward — document → composer →
transcript → document — and `Alt-Left` goes back. Tab is *not* used to
leave the document: Scintilla owns it for indentation. `Alt-J` (Jump to
Agent's Last Edit) moves the document's cursor to whatever the agent most
recently touched, and double-clicking a tool call does the same for that
call. If a call names no part of the document, nothing moves.

## Agents and briefs

Discuss, Draft and Implement all run in the Workbench, over one long-lived
agent process driven with structured JSON on its stdin and stdout
(`--input-format stream-json --output-format stream-json`). The configured
agent CLI is unchanged (`claude`, `codex`, `opencode`, or any command —
Select Agent…), and it still authenticates itself: turbo stores no API key
and there is no billing change.

The mode's brief — its mission, the write-back contract (checkbox plan,
dated Decisions ledger, status transitions, the blocking-discovery
regression), and the domain pack — is sent as the opening message rather
than written to a file and named on the command line. Every launch is
confirmed first.

Closing a Workbench ends its agent session; turbo keeps the agent's own
session id, so reopening resumes the conversation rather than starting
over.

Domain packs resolve: `turbo-scripts/spec-packs/<domain>.md` (project) →
`~/.turbo/spec-packs/<domain>.md` (user) → built-in (`default`, or
`saas-product` which adds monetisation/differentiation/scaling). The pack
is selected by the spec's frontmatter `domain`.

Any MCP client of turboIDE — including the agent in a Workbench pane or the
agent window — can call the `ask_user` tool to raise a native question
wizard (one page per question, Back/Next/Finish, attributed
"From: <agent>"); cancelling returns an explicit sentinel, never empty
answers. The built-in packs tell the agent to prefer that dialog for any
question with enumerable answers, and to name the section it is editing so
the jump keys land somewhere useful.

The Alt-0 agent window is unchanged: it is still a terminal running the
agent interactively, for work that is not about a particular spec.

## The implementation gate

`Implement` runs only when the spec is `reviewed`, every `depends` spec is
`implemented`, and no open questions remain (`- **Q…**` bullets in
Decisions). There is no bypass; the refusal names each blocker. If the
implementing agent hits a blocking discovery, it records why in Decisions
and sets the spec back to `draft` — gates are re-earned, not bypassed.

## Tests

`turbo-test` (gtest, `-DTURBO_BUILD_TESTS=ON`) covers the spec model:
frontmatter, section map, plan progress, gate predicate, template.
`test/pty/spec_identity_test.py` drives the real binary under a pty through
all of the above, including a fake MCP agent speaking through the real
`turboIDE mcp` bridge.
