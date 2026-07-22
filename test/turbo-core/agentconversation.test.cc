// Tests for the agent conversation model (M2 of
// specs/spec-agent-integration.md): event -> transcript mapping and wrapping.

#define Uses_TText
#include <tvision/tv.h>

#include <turbo/agentconversation.h>

#include <gtest/gtest.h>

using turbo::AgentConversation;
using turbo::ConvKind;
using turbo::SpecAgentEvent;
using turbo::SpecAgentEventKind;
using turbo::convItemText;
using turbo::convWrap;

namespace {

SpecAgentEvent ev(SpecAgentEventKind k)
{
    SpecAgentEvent e;
    e.kind = k;
    return e;
}

// Total display width of a UTF-8 string, counted the way the view draws it.
size_t width(const std::string &s)
{
    size_t i = 0, w = 0;
    while (i < s.size())
    {
        size_t before = i;
        if (!TText::next(TStringView(s.data(), s.size()), i, w))
            break;
        if (i == before)
            break;
    }
    return w;
}

} // namespace

// ---- wrapping --------------------------------------------------------------

TEST(ConvWrap, DegenerateWidthsYieldNothing)
{
    EXPECT_TRUE(convWrap("hello", 0).empty());
    EXPECT_TRUE(convWrap("hello", -5).empty());
}

TEST(ConvWrap, ShortTextIsOneLine)
{
    auto out = convWrap("hello world", 40);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], "hello world");
}

TEST(ConvWrap, BreaksAtSpacesAndDropsThem)
{
    auto out = convWrap("aaa bbb ccc", 7);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], "aaa bbb");
    EXPECT_EQ(out[1], "ccc");
}

TEST(ConvWrap, NoLineExceedsTheWidth)
{
    const std::string prose =
        "The Spec Workbench currently places two independent windows side by "
        "side and shares a single app-global agent terminal between specs.";
    for (int w : {5, 12, 20, 33, 80})
    {
        auto out = convWrap(prose, w);
        ASSERT_FALSE(out.empty()) << "width " << w;
        for (const auto &line : out)
            EXPECT_LE(width(line), (size_t) w) << "width " << w << " line: " << line;
    }
}

TEST(ConvWrap, OverlongWordIsHardSplitNotDropped)
{
    auto out = convWrap("supercalifragilistic", 6);
    ASSERT_GE(out.size(), 3u);
    std::string joined;
    for (auto &l : out)
    {
        EXPECT_LE(width(l), 6u);
        joined += l;
    }
    EXPECT_EQ(joined, "supercalifragilistic"); // nothing lost
}

TEST(ConvWrap, HonoursHardNewlines)
{
    auto out = convWrap("one\ntwo\nthree", 40);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0], "one");
    EXPECT_EQ(out[1], "two");
    EXPECT_EQ(out[2], "three");
}

TEST(ConvWrap, BlankLinesSurvive)
{
    auto out = convWrap("a\n\nb", 40);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[1], "");
}

TEST(ConvWrap, StripsTrailingCarriageReturn)
{
    auto out = convWrap("one\r\ntwo", 40);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0], "one");
}

// A wrap that splits a UTF-8 sequence produces mojibake on screen. Rebuilding
// the text from the wrapped rows must reproduce the original bytes exactly.
TEST(ConvWrap, NeverSplitsAUtf8Sequence)
{
    const std::string s = "ααααααααββββββββγγγγγγγγ"; // 2-byte codepoints
    auto out = convWrap(s, 5);
    ASSERT_GE(out.size(), 2u);
    std::string joined;
    for (const auto &l : out)
    {
        EXPECT_LE(width(l), 5u);
        // Every row must itself be walkable as valid UTF-8 to its exact end.
        size_t i = 0, w = 0;
        while (i < l.size())
        {
            size_t before = i;
            ASSERT_TRUE(TText::next(TStringView(l.data(), l.size()), i, w));
            ASSERT_NE(i, before);
        }
        EXPECT_EQ(i, l.size()) << "row ends mid-codepoint: " << l;
        joined += l;
    }
    EXPECT_EQ(joined, s);
}

// ---- item rendering --------------------------------------------------------

TEST(ConvItemText, UserTurnAndErrorCarryMarkers)
{
    turbo::ConvItem u;
    u.kind = ConvKind::UserTurn;
    u.text = "draft the objective";
    EXPECT_EQ(convItemText(u), "> draft the objective");

    turbo::ConvItem e;
    e.kind = ConvKind::Error;
    e.text = "boom";
    EXPECT_EQ(convItemText(e), "! boom");
}

TEST(ConvItemText, ToolUseShowsNameAndInput)
{
    turbo::ConvItem t;
    t.kind = ConvKind::ToolUse;
    t.toolName = "file_text";
    t.toolInput = R"({"path":"/a.md"})";
    std::string s = convItemText(t);
    EXPECT_NE(s.find("file_text"), std::string::npos);
    EXPECT_NE(s.find("/a.md"), std::string::npos);
}

TEST(ConvItemText, ToolUseWithoutInputOmitsEmptyBraces)
{
    turbo::ConvItem t;
    t.kind = ConvKind::ToolUse;
    t.toolName = "project_root";
    t.toolInput = "{}";
    EXPECT_EQ(convItemText(t).find("{}"), std::string::npos);
}

TEST(ConvItemText, ToolResultIsIndented)
{
    turbo::ConvItem r;
    r.kind = ConvKind::ToolResult;
    r.text = "line one";
    EXPECT_EQ(convItemText(r).rfind("    ", 0), 0u);
}

// A tool result is often an entire file; the transcript must not drown in it.
TEST(ConvItemText, LongToolResultIsClipped)
{
    turbo::ConvItem r;
    r.kind = ConvKind::ToolResult;
    for (int i = 0; i < 500; ++i)
        r.text += "line " + std::to_string(i) + "\n";
    std::string s = convItemText(r);
    int newlines = 0;
    for (char c : s)
        if (c == '\n')
            ++newlines;
    EXPECT_LE(newlines, turbo::convToolResultMaxLines + 1);
    EXPECT_NE(s.find("clipped"), std::string::npos);
    EXPECT_EQ(s.find("line 499"), std::string::npos);
}

TEST(ConvItemText, EmptyToolResultStillSaysSomething)
{
    turbo::ConvItem r;
    r.kind = ConvKind::ToolResult;
    EXPECT_NE(convItemText(r).find("no output"), std::string::npos);
}

// ---- tool-call targeting (FR11) --------------------------------------------

namespace {
std::vector<turbo::SpecSection> demoSections()
{
    return {
        {"Background", 5, false},
        {"Objective", 12, true},
        {"Implementation Plan", 40, false},
        {"Decisions", 60, false},
    };
}
} // namespace

TEST(ConvToolTarget, NoInputNoTarget)
{
    EXPECT_EQ(turbo::convToolTargetLine("", demoSections()), -1);
    EXPECT_EQ(turbo::convToolTargetLine("{}", demoSections()), -1);
}

TEST(ConvToolTarget, UnrelatedInputHasNoTarget)
{
    EXPECT_EQ(turbo::convToolTargetLine(R"({"path":"/tmp/other.txt"})",
                                        demoSections()), -1);
}

// An explicit line wins, and converts 1-based wire numbering to the 0-based
// line SCI_GOTOLINE wants.
TEST(ConvToolTarget, ExplicitLineIsConvertedToZeroBased)
{
    EXPECT_EQ(turbo::convToolTargetLine(R"({"path":"/a.md","line":42})",
                                        demoSections()), 41);
    EXPECT_EQ(turbo::convToolTargetLine(R"({"start_line":7})", demoSections()), 6);
    EXPECT_EQ(turbo::convToolTargetLine(R"({"line_number":1})", demoSections()), 0);
    EXPECT_EQ(turbo::convToolTargetLine(R"({"offset":100})", demoSections()), 99);
}

TEST(ConvToolTarget, LineZeroClampsToTheFirstLine)
{
    EXPECT_EQ(turbo::convToolTargetLine(R"({"line":0})", demoSections()), 0);
}

// A quoted value is not a line number; fall through rather than misparse.
TEST(ConvToolTarget, NonNumericLineIsIgnored)
{
    EXPECT_EQ(turbo::convToolTargetLine(R"({"line":"top"})", demoSections()), -1);
}

TEST(ConvToolTarget, AbsurdLineIsRejectedRatherThanJumped)
{
    EXPECT_EQ(turbo::convToolTargetLine(R"({"line":999999999999})",
                                        demoSections()), -1);
}

TEST(ConvToolTarget, SectionNameResolvesToItsHeadingLine)
{
    EXPECT_EQ(turbo::convToolTargetLine(
        R"({"path":"/a.md","content":"# Decisions\n- **D1**"})",
        demoSections()), 60);
}

// "Implementation Plan" must win over any shorter title it contains, and the
// result must not depend on the order sections happen to appear.
TEST(ConvToolTarget, LongestSectionTitleWins)
{
    std::vector<turbo::SpecSection> secs = {
        {"Plan", 3, false},
        {"Implementation Plan", 40, false},
    };
    EXPECT_EQ(turbo::convToolTargetLine(
        R"({"section":"Implementation Plan"})", secs), 40);
}

TEST(ConvToolTarget, ExplicitLineBeatsASectionName)
{
    EXPECT_EQ(turbo::convToolTargetLine(
        R"({"content":"# Decisions","line":9})", demoSections()), 8);
}

TEST(ConvToolTarget, EmptySectionListIsSafe)
{
    EXPECT_EQ(turbo::convToolTargetLine(R"({"content":"# Decisions"})", {}), -1);
}

// ---- event mapping ---------------------------------------------------------

TEST(AgentConversationModel, UserTurnStartsPendingAndIsAcknowledged)
{
    AgentConversation c;
    c.addUserTurn("hello");
    ASSERT_EQ(c.items().size(), 1u);
    EXPECT_TRUE(c.items()[0].pending);

    auto e = ev(SpecAgentEventKind::AssistantText);
    e.text = "hi";
    EXPECT_TRUE(c.addEvent(e));
    EXPECT_FALSE(c.items()[0].pending); // the agent started working
    ASSERT_EQ(c.items().size(), 2u);
    EXPECT_EQ(c.items()[1].kind, ConvKind::Assistant);
}

TEST(AgentConversationModel, SessionStartAndIgnoredAddNothing)
{
    AgentConversation c;
    EXPECT_FALSE(c.addEvent(ev(SpecAgentEventKind::SessionStart)));
    EXPECT_FALSE(c.addEvent(ev(SpecAgentEventKind::Ignored)));
    EXPECT_TRUE(c.empty());
}

TEST(AgentConversationModel, SuccessfulTurnCompleteAddsNoItem)
{
    AgentConversation c;
    auto e = ev(SpecAgentEventKind::TurnComplete);
    e.text = "OK";
    e.isError = false;
    EXPECT_FALSE(c.addEvent(e)); // the prose already arrived as assistant text
    EXPECT_TRUE(c.empty());
}

TEST(AgentConversationModel, FailedTurnCompleteSurfacesAsError)
{
    AgentConversation c;
    auto e = ev(SpecAgentEventKind::TurnComplete);
    e.text = "error_max_turns";
    e.isError = true;
    EXPECT_TRUE(c.addEvent(e));
    ASSERT_EQ(c.items().size(), 1u);
    EXPECT_EQ(c.items()[0].kind, ConvKind::Error);
    EXPECT_EQ(c.items()[0].text, "error_max_turns");
}

TEST(AgentConversationModel, ExitReportsTheCode)
{
    AgentConversation c;
    auto e = ev(SpecAgentEventKind::Exited);
    e.exitCode = 3;
    EXPECT_TRUE(c.addEvent(e));
    ASSERT_EQ(c.items().size(), 1u);
    EXPECT_EQ(c.items()[0].kind, ConvKind::Notice);
    EXPECT_NE(c.items()[0].text.find("3"), std::string::npos);
}

TEST(AgentConversationModel, ToolUseAndResultKeepTheirCorrelationId)
{
    AgentConversation c;
    auto use = ev(SpecAgentEventKind::ToolUse);
    use.toolName = "ask_user";
    use.toolUseId = "toolu_7";
    use.toolInput = R"({"questions":[]})";
    ASSERT_TRUE(c.addEvent(use));

    auto res = ev(SpecAgentEventKind::ToolResult);
    res.toolUseId = "toolu_7";
    res.text = "answered";
    ASSERT_TRUE(c.addEvent(res));

    ASSERT_EQ(c.items().size(), 2u);
    EXPECT_EQ(c.items()[0].toolUseId, "toolu_7");
    EXPECT_EQ(c.items()[1].toolUseId, "toolu_7");
}

// ---- layout ----------------------------------------------------------------

TEST(AgentConversationLayout, EmptyTranscriptYieldsNoLines)
{
    AgentConversation c;
    EXPECT_TRUE(c.layout(40).empty());
}

TEST(AgentConversationLayout, DegenerateWidthYieldsNoLines)
{
    AgentConversation c;
    c.addNotice("hi");
    EXPECT_TRUE(c.layout(0).empty());
}

TEST(AgentConversationLayout, LinesPointBackAtTheirItem)
{
    AgentConversation c;
    c.addUserTurn("first");
    c.addNotice("second");
    const auto &lines = c.layout(40);
    ASSERT_FALSE(lines.empty());
    for (const auto &l : lines)
        EXPECT_LT(l.item, c.items().size());
    // The first row of the first item carries the prefix.
    EXPECT_TRUE(lines[0].first);
    EXPECT_EQ(lines[0].kind, ConvKind::UserTurn);
}

TEST(AgentConversationLayout, ItemsAreSeparatedByABlankRow)
{
    AgentConversation c;
    c.addNotice("one");
    c.addNotice("two");
    const auto &lines = c.layout(40);
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[1].text, ""); // separator, not a first row
    EXPECT_FALSE(lines[1].first);
}

TEST(AgentConversationLayout, RewrapsWhenTheWidthChanges)
{
    AgentConversation c;
    c.addNotice("aaa bbb ccc ddd eee fff");
    size_t wide = c.layout(80).size();
    size_t narrow = c.layout(10).size();
    EXPECT_GT(narrow, wide);
    EXPECT_EQ(c.layout(80).size(), wide); // and back again
}

TEST(AgentConversationLayout, RelaidOutWhenItemsChange)
{
    AgentConversation c;
    c.addNotice("one");
    size_t before = c.layout(40).size();
    c.addNotice("two");
    EXPECT_GT(c.layout(40).size(), before);
}

// Acknowledging a pending turn must invalidate the cache, or the view keeps
// drawing the turn as still in flight.
TEST(AgentConversationLayout, PendingFlagChangeInvalidatesTheCache)
{
    AgentConversation c;
    c.addUserTurn("hello");
    ASSERT_TRUE(c.layout(40)[0].pending);
    c.acknowledgePending();
    EXPECT_FALSE(c.layout(40)[0].pending);
}

TEST(AgentConversationModel, ClearEmptiesEverything)
{
    AgentConversation c;
    c.addNotice("one");
    c.layout(40);
    c.clear();
    EXPECT_TRUE(c.empty());
    EXPECT_TRUE(c.layout(40).empty());
}
