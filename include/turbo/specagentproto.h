#ifndef TURBO_SPECAGENTPROTO_H
#define TURBO_SPECAGENTPROTO_H

#include <string>
#include <string_view>
#include <vector>

namespace turbo {

// The wire protocol of a headless coding agent driven with stream-json in and
// stream-json out: one long-lived child process, user turns written to its
// stdin as JSON, agent events read from its stdout as newline-delimited JSON.
//
// This header is the pure half -- argv assembly, turn encoding, and event
// parsing -- so it can be unit tested without spawning anything. The threaded
// transport that uses it is SpecAgentSession (source/turbo/specagentsession.h).
//
// Observed event stream for one turn (Claude Code 2.1.x):
//   {"type":"system","subtype":"init","session_id":...,"model":...}
//   {"type":"assistant","message":{"content":[{"type":"text",...}|
//                                             {"type":"tool_use",...}]}}
//   {"type":"user","message":{"content":[{"type":"tool_result",...}]}}
//   {"type":"result","subtype":"success","is_error":false,"result":"..."}
// The child stays alive after 'result' and accepts the next turn on stdin,
// which is what makes one session per Workbench window possible.

enum class SpecAgentEventKind
{
    SessionStart,  // system/init: sessionId + model are set
    AssistantText, // a text block from an assistant message
    ToolUse,       // a tool_use block: toolName, toolInput, toolUseId
    ToolResult,    // a tool_result block: toolUseId, text, isError
    TurnComplete,  // result: text is the final answer, isError on failure
    Error,         // a malformed line or an agent-reported error
    Exited,        // the child process ended (synthesised, never parsed)
    Ignored,       // a well-formed event this layer does not surface
};

struct SpecAgentEvent
{
    SpecAgentEventKind kind {SpecAgentEventKind::Ignored};
    std::string text;      // AssistantText / ToolResult / TurnComplete / Error
    std::string toolName;  // ToolUse
    std::string toolInput; // ToolUse: the input object as compact JSON
    std::string toolUseId; // ToolUse / ToolResult (correlates the two)
    std::string sessionId; // SessionStart, and any event that carries one
    std::string model;     // SessionStart
    bool isError {false};  // ToolResult / TurnComplete
    int exitCode {0};      // Exited
};

// Parse one NDJSON line into zero or more events. A single assistant message
// can carry several content blocks (prose plus two tool calls, say), so this
// appends rather than returning one event. Malformed input yields a single
// Error event -- agent stdout is untrusted, so this never throws and never
// assumes a field is present.
void parseSpecAgentLine(std::string_view line, std::vector<SpecAgentEvent> &out);

// Convenience wrapper over parseSpecAgentLine.
std::vector<SpecAgentEvent> parseSpecAgentLine(std::string_view line);

// The argv for the agent child. 'command' is the configured agent command
// line ("claude", or a user-supplied command with its own flags), split on
// whitespace; the stream-json flags are appended, plus --resume when
// 'resumeSessionId' is non-empty.
//
// The prompt is deliberately NOT an argument: turns go over stdin as JSON
// (see specAgentTurnJson). The previous design shell-quoted the prompt into
// the command line, but nothing ever ran a shell -- the string was split on
// whitespace and passed to execvp, so the agent received the prompt as a
// dozen separate argv entries with stray quote characters.
std::vector<std::string> specAgentArgv(std::string_view command,
                                       std::string_view resumeSessionId = {});

// The program name (argv[0]) for the above -- the first whitespace-separated
// token of 'command'. Empty if 'command' is blank.
std::string specAgentProgram(std::string_view command);

// One user turn, encoded as a stream-json line (no trailing newline).
std::string specAgentTurnJson(std::string_view text);

} // namespace turbo

#endif // TURBO_SPECAGENTPROTO_H
