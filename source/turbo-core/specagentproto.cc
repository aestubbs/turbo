#include <turbo/specagentproto.h>

#include <nlohmann/json.hpp>

using Json = nlohmann::json;

namespace turbo {

// Read a string field without throwing or asserting its presence: agent
// stdout is untrusted input, so a wrong type is treated as absent.
static std::string str(const Json &j, const char *key)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_string())
        return {};
    return it->get<std::string>();
}

static SpecAgentEvent errorEvent(std::string text)
{
    SpecAgentEvent e;
    e.kind = SpecAgentEventKind::Error;
    e.text = std::move(text);
    return e;
}

// One content block of an assistant/user message -> at most one event.
static void parseBlock(const Json &block, const std::string &sessionId,
                       std::vector<SpecAgentEvent> &out)
{
    if (!block.is_object())
        return;
    std::string type = str(block, "type");
    if (type == "text")
    {
        std::string text = str(block, "text");
        if (text.empty())
            return;
        SpecAgentEvent e;
        e.kind = SpecAgentEventKind::AssistantText;
        e.text = std::move(text);
        e.sessionId = sessionId;
        out.push_back(std::move(e));
    }
    else if (type == "tool_use")
    {
        SpecAgentEvent e;
        e.kind = SpecAgentEventKind::ToolUse;
        e.toolName = str(block, "name");
        e.toolUseId = str(block, "id");
        auto input = block.find("input");
        if (input != block.end() && !input->is_null())
            e.toolInput = input->dump();
        e.sessionId = sessionId;
        out.push_back(std::move(e));
    }
    else if (type == "tool_result")
    {
        SpecAgentEvent e;
        e.kind = SpecAgentEventKind::ToolResult;
        e.toolUseId = str(block, "tool_use_id");
        auto err = block.find("is_error");
        e.isError = err != block.end() && err->is_boolean() && err->get<bool>();
        // 'content' is either a plain string or an array of text blocks.
        auto content = block.find("content");
        if (content != block.end())
        {
            if (content->is_string())
                e.text = content->get<std::string>();
            else if (content->is_array())
                for (const auto &c : *content)
                    if (c.is_object() && str(c, "type") == "text")
                        e.text += str(c, "text");
        }
        e.sessionId = sessionId;
        out.push_back(std::move(e));
    }
}

void parseSpecAgentLine(std::string_view line, std::vector<SpecAgentEvent> &out)
{
    // Blank lines are normal framing slack, not errors.
    bool blank = true;
    for (char c : line)
        if (c != ' ' && c != '\t' && c != '\r')
        {
            blank = false;
            break;
        }
    if (blank)
        return;

    // Project convention: never-throwing parse, then check is_discarded().
    Json j = Json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object())
    {
        out.push_back(errorEvent("Unparseable agent output: " +
                                 std::string(line.substr(0, 200))));
        return;
    }

    std::string type = str(j, "type");
    std::string sessionId = str(j, "session_id");

    if (type == "system")
    {
        if (str(j, "subtype") != "init")
        {
            out.push_back({SpecAgentEventKind::Ignored});
            return;
        }
        SpecAgentEvent e;
        e.kind = SpecAgentEventKind::SessionStart;
        e.sessionId = sessionId;
        e.model = str(j, "model");
        out.push_back(std::move(e));
        return;
    }

    if (type == "assistant" || type == "user")
    {
        auto msg = j.find("message");
        if (msg == j.end() || !msg->is_object())
        {
            out.push_back({SpecAgentEventKind::Ignored});
            return;
        }
        auto content = msg->find("content");
        if (content == msg->end())
        {
            out.push_back({SpecAgentEventKind::Ignored});
            return;
        }
        if (content->is_array())
            for (const auto &block : *content)
                parseBlock(block, sessionId, out);
        else if (content->is_string() && type == "assistant")
        {
            SpecAgentEvent e;
            e.kind = SpecAgentEventKind::AssistantText;
            e.text = content->get<std::string>();
            e.sessionId = sessionId;
            out.push_back(std::move(e));
        }
        return;
    }

    if (type == "result")
    {
        SpecAgentEvent e;
        e.kind = SpecAgentEventKind::TurnComplete;
        e.sessionId = sessionId;
        auto err = j.find("is_error");
        e.isError = err != j.end() && err->is_boolean() && err->get<bool>();
        // 'result' is the final assistant text on success; on failure the
        // subtype (e.g. error_max_turns) is the only description available.
        auto res = j.find("result");
        if (res != j.end() && res->is_string())
            e.text = res->get<std::string>();
        if (e.text.empty())
            e.text = str(j, "subtype");
        if (str(j, "subtype") != "success")
            e.isError = true;
        out.push_back(std::move(e));
        return;
    }

    // Everything else (rate_limit_event, hook chatter, stream deltas) is
    // well-formed but not something this layer surfaces.
    out.push_back({SpecAgentEventKind::Ignored});
}

std::vector<SpecAgentEvent> parseSpecAgentLine(std::string_view line)
{
    std::vector<SpecAgentEvent> out;
    parseSpecAgentLine(line, out);
    return out;
}

// Split a configured command line on whitespace. Only ever applied to the
// agent command from settings -- never to a prompt (see the header).
static std::vector<std::string> splitTokens(std::string_view s)
{
    std::vector<std::string> out;
    size_t i = 0, n = s.size();
    while (i < n)
    {
        while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
        size_t start = i;
        while (i < n && s[i] != ' ' && s[i] != '\t') ++i;
        if (i > start)
            out.emplace_back(s.substr(start, i - start));
    }
    return out;
}

std::string specAgentProgram(std::string_view command)
{
    auto toks = splitTokens(command);
    return toks.empty() ? std::string {} : toks.front();
}

std::vector<std::string> specAgentArgv(std::string_view command,
                                       std::string_view resumeSessionId)
{
    auto toks = splitTokens(command);
    std::vector<std::string> argv;
    if (toks.empty())
        return argv;
    // Drop argv[0]; Process::start takes the program separately.
    argv.assign(toks.begin() + 1, toks.end());
    argv.emplace_back("-p");
    argv.emplace_back("--input-format");
    argv.emplace_back("stream-json");
    argv.emplace_back("--output-format");
    argv.emplace_back("stream-json");
    argv.emplace_back("--verbose"); // required for stream-json output
    if (!resumeSessionId.empty())
    {
        argv.emplace_back("--resume");
        argv.emplace_back(resumeSessionId);
    }
    return argv;
}

std::string specAgentTurnJson(std::string_view text)
{
    Json block {{"type", "text"}, {"text", std::string(text)}};
    Json msg {{"role", "user"}, {"content", Json::array({block})}};
    Json line {{"type", "user"}, {"message", msg}};
    return line.dump();
}

} // namespace turbo
