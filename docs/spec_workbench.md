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

`Spec Workbench` (command palette, or D in the Manager) tiles the spec's
editor (left, with a live "Sections n/m drafted | empty: …" strip) beside
the agent terminal (right). The conversation is about the document: answers
are distilled into sections, and you can always type into the spec directly
— agent writes reload into the editor automatically.

## Agents and briefs

Discuss/Draft/Implement launch the configured agent CLI (`claude`, `codex`,
`opencode`, or any command — Select Agent…) with a brief written to
`.turbo/spec-sessions/<spec>-<mode>.md`: the mode's mission, the write-back
contract (checkbox plan, dated Decisions ledger, status transitions, the
blocking-discovery regression), and the domain pack. Every launch is
confirmed first.

Domain packs resolve: `turbo-scripts/spec-packs/<domain>.md` (project) →
`~/.turbo/spec-packs/<domain>.md` (user) → built-in (`default`, or
`saas-product` which adds monetisation/differentiation/scaling). The pack
is selected by the spec's frontmatter `domain`.

Any MCP client of turboIDE — including the agent in the agent window — can
call the `ask_user` tool to raise a native question wizard (one page per
question, Back/Next/Finish, attributed "From: <agent>"); cancelling returns
an explicit sentinel, never empty answers.

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
