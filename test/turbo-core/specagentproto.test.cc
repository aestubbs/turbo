// Tests for the headless spec-agent wire protocol (M1 of
// specs/spec-agent-integration.md). Fixtures are trimmed from a real
// Claude Code 2.1.x stream-json session, not invented.

#include <turbo/specagentproto.h>

#include <gtest/gtest.h>

#include <algorithm>

using turbo::SpecAgentEvent;
using turbo::SpecAgentEventKind;
using turbo::parseSpecAgentLine;

namespace {

// Events this layer actually surfaces (Ignored dropped), for terser asserts.
std::vector<SpecAgentEvent> surfaced(std::string_view line)
{
    std::vector<SpecAgentEvent> out;
    for (auto &e : parseSpecAgentLine(line))
        if (e.kind != SpecAgentEventKind::Ignored)
            out.push_back(e);
    return out;
}

} // namespace

// ---- argv assembly (D7: the prompt must never reach argv) -------------------

TEST(SpecAgentArgv, AddsStreamJsonFlags)
{
    auto argv = turbo::specAgentArgv("claude");
    EXPECT_EQ(turbo::specAgentProgram("claude"), "claude");
    // --verbose is required for stream-json output, not cosmetic.
    EXPECT_NE(std::find(argv.begin(), argv.end(), "--verbose"), argv.end());
    EXPECT_NE(std::find(argv.begin(), argv.end(), "-p"), argv.end());
    int streamJson = 0;
    for (auto &a : argv)
        if (a == "stream-json")
            ++streamJson;
    EXPECT_EQ(streamJson, 2); // one for --input-format, one for --output-format
}

TEST(SpecAgentArgv, KeepsExtraTokensFromTheConfiguredCommand)
{
    auto argv = turbo::specAgentArgv("my-agent --flag value");
    EXPECT_EQ(turbo::specAgentProgram("my-agent --flag value"), "my-agent");
    ASSERT_GE(argv.size(), 2u);
    EXPECT_EQ(argv[0], "--flag");
    EXPECT_EQ(argv[1], "value");
}

TEST(SpecAgentArgv, ResumePassesSessionId)
{
    auto argv = turbo::specAgentArgv("claude", "1e1597d4-5196-4983-aeba");
    auto it = std::find(argv.begin(), argv.end(), "--resume");
    ASSERT_NE(it, argv.end());
    ASSERT_NE(it + 1, argv.end());
    EXPECT_EQ(*(it + 1), "1e1597d4-5196-4983-aeba");
}

TEST(SpecAgentArgv, NoResumeFlagWhenNotResuming)
{
    auto argv = turbo::specAgentArgv("claude");
    EXPECT_EQ(std::find(argv.begin(), argv.end(), "--resume"), argv.end());
}

TEST(SpecAgentArgv, EmptyCommandYieldsNothing)
{
    EXPECT_TRUE(turbo::specAgentProgram("   ").empty());
    EXPECT_TRUE(turbo::specAgentArgv("   ").empty());
}

// The regression guard for the bug this milestone retires: the old path
// shell-quoted the prompt into a command line that was then split on
// whitespace and handed to execvp, so a multi-word prompt arrived as many
// arguments. Prompts must not appear in argv at all now.
TEST(SpecAgentArgv, PromptTextNeverAppearsInArgv)
{
    const std::string prompt =
        "Read and follow the instructions in '/tmp/brief.md'. "
        "The spec is '/tmp/specs/thing.md'.";
    auto argv = turbo::specAgentArgv("claude");
    for (const auto &a : argv)
    {
        EXPECT_EQ(a.find("Read and follow"), std::string::npos);
        EXPECT_EQ(a.find("brief.md"), std::string::npos);
        EXPECT_EQ(a.find('\''), std::string::npos) << "stray shell quoting in " << a;
    }
    // It travels as one JSON turn instead, intact and unsplit.
    std::string turn = turbo::specAgentTurnJson(prompt);
    EXPECT_NE(turn.find("Read and follow the instructions"), std::string::npos);
}

// ---- turn encoding ---------------------------------------------------------

TEST(SpecAgentTurn, IsOneLineOfStreamJson)
{
    std::string turn = turbo::specAgentTurnJson("hello");
    EXPECT_EQ(turn.find('\n'), std::string::npos); // caller appends the newline
    EXPECT_NE(turn.find("\"type\":\"user\""), std::string::npos);
    EXPECT_NE(turn.find("\"role\":\"user\""), std::string::npos);
    EXPECT_NE(turn.find("\"text\":\"hello\""), std::string::npos);
}

TEST(SpecAgentTurn, EscapesQuotesAndNewlines)
{
    std::string turn = turbo::specAgentTurnJson("say \"hi\"\nthen stop");
    EXPECT_EQ(turn.find('\n'), std::string::npos); // the newline is escaped
    EXPECT_NE(turn.find("\\\"hi\\\""), std::string::npos);
    // Round-trips through the parser we hand it to.
    auto ev = surfaced(turn);
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].kind, SpecAgentEventKind::AssistantText); // user echo shape
    EXPECT_EQ(ev[0].text, "say \"hi\"\nthen stop");
}

// ---- event parsing ---------------------------------------------------------

TEST(SpecAgentParse, SystemInitYieldsSessionStart)
{
    auto ev = surfaced(
        R"({"type":"system","subtype":"init","cwd":"/tmp",)"
        R"("session_id":"1e1597d4-5196","model":"claude-opus-4-8[1m]",)"
        R"("permissionMode":"default","apiKeySource":"none"})");
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].kind, SpecAgentEventKind::SessionStart);
    EXPECT_EQ(ev[0].sessionId, "1e1597d4-5196");
    EXPECT_EQ(ev[0].model, "claude-opus-4-8[1m]");
}

TEST(SpecAgentParse, NonInitSystemEventsAreIgnored)
{
    EXPECT_TRUE(surfaced(
        R"({"type":"system","subtype":"hook_started","hook_name":"SessionStart"})").empty());
    EXPECT_TRUE(surfaced(
        R"({"type":"rate_limit_event","rate_limit_info":{"status":"allowed"}})").empty());
}

TEST(SpecAgentParse, AssistantTextBlock)
{
    auto ev = surfaced(
        R"({"type":"assistant","message":{"role":"assistant","content":)"
        R"([{"type":"text","text":"OK"}]},"session_id":"s1"})");
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].kind, SpecAgentEventKind::AssistantText);
    EXPECT_EQ(ev[0].text, "OK");
    EXPECT_EQ(ev[0].sessionId, "s1");
}

// One assistant message can carry prose and several tool calls at once, which
// is why parsing appends events rather than returning one.
TEST(SpecAgentParse, OneMessageCanYieldSeveralEvents)
{
    auto ev = surfaced(
        R"({"type":"assistant","message":{"content":[)"
        R"({"type":"text","text":"Reading the spec."},)"
        R"({"type":"tool_use","id":"toolu_1","name":"file_text","input":{"path":"/a.md"}},)"
        R"({"type":"tool_use","id":"toolu_2","name":"ask_user","input":{"questions":[]}}]},)"
        R"("session_id":"s1"})");
    ASSERT_EQ(ev.size(), 3u);
    EXPECT_EQ(ev[0].kind, SpecAgentEventKind::AssistantText);
    EXPECT_EQ(ev[1].kind, SpecAgentEventKind::ToolUse);
    EXPECT_EQ(ev[1].toolName, "file_text");
    EXPECT_EQ(ev[1].toolUseId, "toolu_1");
    EXPECT_NE(ev[1].toolInput.find("/a.md"), std::string::npos);
    EXPECT_EQ(ev[2].kind, SpecAgentEventKind::ToolUse);
    EXPECT_EQ(ev[2].toolName, "ask_user");
}

TEST(SpecAgentParse, ToolResultFromUserMessage)
{
    auto ev = surfaced(
        R"({"type":"user","message":{"role":"user","content":)"
        R"([{"type":"tool_result","tool_use_id":"toolu_1","content":"file body"}]}})");
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].kind, SpecAgentEventKind::ToolResult);
    EXPECT_EQ(ev[0].toolUseId, "toolu_1");
    EXPECT_EQ(ev[0].text, "file body");
    EXPECT_FALSE(ev[0].isError);
}

TEST(SpecAgentParse, ToolResultCarriesErrorFlagAndBlockContent)
{
    auto ev = surfaced(
        R"({"type":"user","message":{"content":[{"type":"tool_result",)"
        R"("tool_use_id":"toolu_9","is_error":true,)"
        R"("content":[{"type":"text","text":"no such file"}]}]}})");
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].kind, SpecAgentEventKind::ToolResult);
    EXPECT_TRUE(ev[0].isError);
    EXPECT_EQ(ev[0].text, "no such file");
}

TEST(SpecAgentParse, ResultEndsTheTurn)
{
    auto ev = surfaced(
        R"({"type":"result","subtype":"success","is_error":false,)"
        R"("result":"OK","session_id":"s1","num_turns":1,"total_cost_usd":0.07})");
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].kind, SpecAgentEventKind::TurnComplete);
    EXPECT_FALSE(ev[0].isError);
    EXPECT_EQ(ev[0].text, "OK");
    EXPECT_EQ(ev[0].sessionId, "s1");
}

TEST(SpecAgentParse, FailedResultIsAnError)
{
    auto ev = surfaced(
        R"({"type":"result","subtype":"error_max_turns","is_error":true,)"
        R"("session_id":"s1"})");
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].kind, SpecAgentEventKind::TurnComplete);
    EXPECT_TRUE(ev[0].isError);
    EXPECT_EQ(ev[0].text, "error_max_turns"); // subtype describes the failure
}

// A subtype other than "success" means failure even if is_error is absent.
TEST(SpecAgentParse, NonSuccessSubtypeIsErrorWithoutTheFlag)
{
    auto ev = surfaced(R"({"type":"result","subtype":"error_during_execution"})");
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_TRUE(ev[0].isError);
}

// ---- hostile / malformed input --------------------------------------------

TEST(SpecAgentParse, BlankLinesAreNotErrors)
{
    EXPECT_TRUE(parseSpecAgentLine("").empty());
    EXPECT_TRUE(parseSpecAgentLine("   \t ").empty());
    EXPECT_TRUE(parseSpecAgentLine("\r").empty());
}

TEST(SpecAgentParse, GarbageYieldsAnErrorEventNotACrash)
{
    auto ev = surfaced("this is not json at all");
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].kind, SpecAgentEventKind::Error);
    EXPECT_NE(ev[0].text.find("not json"), std::string::npos);
}

TEST(SpecAgentParse, TruncatedJsonYieldsAnError)
{
    auto ev = surfaced(R"({"type":"assistant","message":{"content":[{"type":"te)");
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].kind, SpecAgentEventKind::Error);
}

TEST(SpecAgentParse, NonObjectJsonYieldsAnError)
{
    EXPECT_EQ(surfaced("[1,2,3]").at(0).kind, SpecAgentEventKind::Error);
    EXPECT_EQ(surfaced("\"bare string\"").at(0).kind, SpecAgentEventKind::Error);
}

// Wrong types where strings are expected must be treated as absent, not
// trusted and not fatal -- agent stdout is untrusted input.
TEST(SpecAgentParse, WrongFieldTypesDegradeGracefully)
{
    auto ev = surfaced(
        R"({"type":"system","subtype":"init","session_id":42,"model":null})");
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].kind, SpecAgentEventKind::SessionStart);
    EXPECT_TRUE(ev[0].sessionId.empty());
    EXPECT_TRUE(ev[0].model.empty());
}

TEST(SpecAgentParse, MessageWithoutContentIsIgnoredNotFatal)
{
    EXPECT_TRUE(surfaced(R"({"type":"assistant","message":{}})").empty());
    EXPECT_TRUE(surfaced(R"({"type":"assistant"})").empty());
    EXPECT_TRUE(surfaced(R"({"type":"assistant","message":"oops"})").empty());
}

TEST(SpecAgentParse, UnknownEventTypesAreIgnored)
{
    EXPECT_TRUE(surfaced(R"({"type":"some_future_event","payload":{}})").empty());
    EXPECT_TRUE(surfaced(R"({"no_type_field":true})").empty());
}

TEST(SpecAgentParse, EmptyTextBlocksAreDropped)
{
    EXPECT_TRUE(surfaced(
        R"({"type":"assistant","message":{"content":[{"type":"text","text":""}]}})").empty());
}
