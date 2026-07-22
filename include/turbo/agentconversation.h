#ifndef TURBO_AGENTCONVERSATION_H
#define TURBO_AGENTCONVERSATION_H

#include <turbo/specagentproto.h>
#include <turbo/specmodel.h>

#include <string>
#include <vector>

namespace turbo {

// The transcript behind the Workbench's right pane (M2 of
// specs/spec-agent-integration.md): agent events in, wrapped display lines
// out. Pure model -- no views, no tvision widgets -- so the wrapping and the
// event-to-item mapping can be unit tested.
//
// The pane is a conversation, not a terminal: each item knows what kind of
// thing it is, so the view can colour it, indent it, and (from M4) attach
// behaviour to it. That is the whole point of the structured transport.

enum class ConvKind
{
    UserTurn,   // something the user sent
    Assistant,  // the agent's prose
    ToolUse,    // the agent called a tool
    ToolResult, // what the tool returned
    Error,      // a transport or agent error
    Notice,     // turbo's own commentary ("agent exited", "resumed session")
};

struct ConvItem
{
    ConvKind kind {ConvKind::Notice};
    std::string text;      // prose, tool result body, or notice
    std::string toolName;  // ToolUse
    std::string toolInput; // ToolUse: input object as compact JSON
    std::string toolUseId; // ToolUse / ToolResult
    bool isError {false};  // ToolResult / Error
    bool pending {false};  // UserTurn: sent, not yet acknowledged (FR12)
};

// One laid-out display row. The view draws these directly; 'item' points back
// so a click can resolve to the thing that produced the row.
struct ConvLine
{
    std::string text;
    ConvKind kind {ConvKind::Notice};
    size_t item {0};
    bool first {false};   // first row of its item (carries the prefix)
    bool pending {false};
    bool isError {false};
};

// Tool results are frequently whole files. Keeping them intact would bury the
// conversation, so they are clipped for display; the agent still saw all of it.
constexpr int convToolResultMaxLines = 6;
constexpr int convToolResultMaxChars = 400;

class AgentConversation
{
public:
    // Record a turn the user just sent. Marked pending until acknowledged.
    void addUserTurn(std::string text);
    // Clear the pending flag on the most recent user turn (the agent has
    // started working: its first event of the turn arrived).
    void acknowledgePending();
    // Map one agent event onto the transcript. Ignored/SessionStart events
    // that carry nothing to show are dropped. Returns true if an item was
    // added, so the caller can decide whether to scroll.
    bool addEvent(const SpecAgentEvent &e);
    void addNotice(std::string text);
    void clear();

    const std::vector<ConvItem> &items() const noexcept { return items_; }
    bool empty() const noexcept { return items_.empty(); }

    // Lay the transcript out for a pane 'width' columns wide. Cached: repeated
    // calls at the same width after no changes return the same vector without
    // re-wrapping. A width < 1 yields no lines.
    const std::vector<ConvLine> &layout(int width);

private:
    std::vector<ConvItem> items_;
    std::vector<ConvLine> lines_;
    int laidOutWidth_ {-1};
    unsigned long revision_ {0};      // bumped on every mutation
    unsigned long laidOutRevision_ {~0UL};
};

// The display text for an item, before wrapping (prefix included). Exposed for
// tests and so the view and the model cannot disagree about it.
std::string convItemText(const ConvItem &item);

// Where in the spec a tool call was aimed (FR11), so clicking it can move the
// document's cursor there. 'toolInput' is the call's input object as compact
// JSON; 'sections' are the spec's headings as specSections() reports them.
//
// Two signals, in order:
//  1. an explicit line/offset number in the input (1-based on the wire,
//     returned 0-based for SCI_GOTOLINE);
//  2. otherwise a canonical section heading named anywhere in the input --
//     the agent writing to "# Decisions" should take you to Decisions.
// Returns -1 when the call names no part of the document.
long convToolTargetLine(const std::string &toolInput,
                        const std::vector<SpecSection> &sections);

// Wrap 'text' to 'width' display columns, breaking at spaces where possible
// and splitting over-long words on codepoint boundaries. Never splits a UTF-8
// sequence. 'width' < 1 yields an empty result.
std::vector<std::string> convWrap(const std::string &text, int width);

} // namespace turbo

#endif // TURBO_AGENTCONVERSATION_H
