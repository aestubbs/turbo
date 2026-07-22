#define Uses_TText
#include <tvision/tv.h>

#include <turbo/agentconversation.h>

namespace turbo {

// Advance one codepoint from 'i', reporting its display width. TText is the
// same primitive tvision draws with, so wrapping agrees with rendering for
// wide glyphs and never splits a UTF-8 sequence.
// 'index' is absolute within 'text' and is advanced in place; 'width' is
// incremented by the character's width, so start it at zero to read just this
// one. Returns false at the end of the string or on a byte that makes no
// progress (malformed input must not spin the caller's loop).
static bool nextCodepoint(const std::string &s, size_t &i, size_t &w)
{
    size_t before = i;
    size_t width = 0;
    if (!TText::next(TStringView(s.data(), s.size()), i, width))
        return false;
    if (i == before)
        return false;
    w = width;
    return true;
}

std::vector<std::string> convWrap(const std::string &text, int width)
{
    std::vector<std::string> out;
    if (width < 1)
        return out;

    // Honour hard newlines first; wrap each paragraph independently.
    size_t start = 0;
    while (start <= text.size())
    {
        size_t nl = text.find('\n', start);
        std::string para = text.substr(start, nl == std::string::npos
                                              ? std::string::npos : nl - start);
        if (!para.empty() && para.back() == '\r')
            para.pop_back();

        // Leading spaces are meaningful -- tool results are indented to read
        // as subordinate to their call -- so they are held and re-applied to
        // every row of the paragraph rather than wrapped away.
        size_t ind = 0;
        while (ind < para.size() && para[ind] == ' ')
            ++ind;
        std::string indent = para.substr(0, ind);
        size_t avail = (size_t) width > ind ? (size_t) width - ind : 1;
        if ((size_t) width <= ind) // pathologically narrow pane
            indent.clear();
        std::string body = para.substr(ind);

        if (body.empty())
            out.emplace_back();
        else
        {
            std::string line;  // content of the current row, without the indent
            size_t lineW = 0;
            auto flush = [&] {
                out.push_back(indent + line);
                line.clear();
                lineW = 0;
            };
            size_t i = 0;
            while (i < body.size())
            {
                // The run of spaces separating the previous word from this one.
                size_t sepW = 0;
                while (i < body.size() && body[i] == ' ')
                {
                    ++i;
                    ++sepW;
                }
                if (i >= body.size())
                    break;
                size_t wordStart = i, wordW = 0;
                while (i < body.size() && body[i] != ' ')
                {
                    size_t w = 0;
                    if (!nextCodepoint(body, i, w))
                        break; // malformed tail; stop rather than spin
                    wordW += w;
                }
                if (i == wordStart)
                    break;
                std::string word = body.substr(wordStart, i - wordStart);

                // Break before the word rather than after the separator, so
                // the space is dropped at the break instead of being trailed.
                if (lineW > 0 && lineW + sepW + wordW > avail)
                    flush();
                if (lineW > 0)
                {
                    line.append(sepW, ' ');
                    lineW += sepW;
                }
                if (wordW > avail) // longer than the pane: split it up
                {
                    size_t j = 0;
                    while (j < word.size())
                    {
                        size_t chStart = j, w = 0;
                        if (!nextCodepoint(word, j, w))
                            break;
                        if (lineW > 0 && lineW + w > avail)
                            flush();
                        line += word.substr(chStart, j - chStart);
                        lineW += w;
                    }
                }
                else
                {
                    line += word;
                    lineW += wordW;
                }
            }
            if (!line.empty())
                flush();
        }

        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    return out;
}

// Clip a tool result for display. The agent received the whole thing; the
// transcript only needs enough to show what came back.
static std::string clipResult(const std::string &s)
{
    std::string out;
    int lines = 0;
    size_t i = 0;
    while (i < s.size() && lines < convToolResultMaxLines &&
           (int) out.size() < convToolResultMaxChars)
    {
        size_t nl = s.find('\n', i);
        std::string seg = s.substr(i, nl == std::string::npos
                                      ? std::string::npos : nl - i);
        if (!out.empty())
            out += '\n';
        out += seg;
        ++lines;
        if (nl == std::string::npos)
        {
            i = s.size();
            break;
        }
        i = nl + 1;
    }
    if ((int) out.size() > convToolResultMaxChars)
        out.resize(convToolResultMaxChars);
    if (i < s.size())
        out += "\n… (clipped)";
    return out;
}

std::string convItemText(const ConvItem &item)
{
    switch (item.kind)
    {
        case ConvKind::UserTurn:
            return "> " + item.text;
        case ConvKind::Assistant:
            return item.text;
        case ConvKind::ToolUse:
        {
            // ASCII markers only: glyph coverage cannot be detected at
            // runtime, so colour and indentation carry the distinction.
            std::string s = "* " + (item.toolName.empty() ? std::string("tool")
                                                          : item.toolName);
            if (!item.toolInput.empty() && item.toolInput != "{}")
            {
                std::string in = item.toolInput;
                if (in.size() > 120)
                    in = in.substr(0, 120) + "…";
                s += "  " + in;
            }
            return s;
        }
        case ConvKind::ToolResult:
        {
            std::string body = clipResult(item.text);
            if (body.empty())
                body = item.isError ? "(error)" : "(no output)";
            std::string out;
            // Indent every line so results read as subordinate to the call.
            size_t i = 0;
            while (i <= body.size())
            {
                size_t nl = body.find('\n', i);
                std::string seg = body.substr(i, nl == std::string::npos
                                                 ? std::string::npos : nl - i);
                if (!out.empty())
                    out += '\n';
                out += "    " + seg;
                if (nl == std::string::npos)
                    break;
                i = nl + 1;
            }
            return out;
        }
        case ConvKind::Error:
            return "! " + item.text;
        case ConvKind::Notice:
        default:
            return "- " + item.text;
    }
}

long convToolTargetLine(const std::string &toolInput,
                        const std::vector<SpecSection> &sections)
{
    if (toolInput.empty())
        return -1;

    // 1. An explicit line number. Parsed off the raw JSON text rather than
    //    through a JSON DOM: the field may sit at any depth and under several
    //    names, and this layer must not care which tool produced it.
    static const char *lineKeys[] = {"\"line\"", "\"start_line\"",
                                     "\"line_number\"", "\"offset\""};
    for (const char *key : lineKeys)
    {
        size_t k = toolInput.find(key);
        if (k == std::string::npos)
            continue;
        size_t i = k + std::char_traits<char>::length(key);
        while (i < toolInput.size() && (toolInput[i] == ':' || toolInput[i] == ' '))
            ++i;
        if (i >= toolInput.size() || toolInput[i] < '0' || toolInput[i] > '9')
            continue; // not a number (e.g. a quoted value) -- ignore
        long n = 0;
        while (i < toolInput.size() && toolInput[i] >= '0' && toolInput[i] <= '9')
        {
            n = n * 10 + (toolInput[i] - '0');
            ++i;
            if (n > 100000000L) // absurd: treat as no target rather than jump
                return -1;
        }
        return n > 0 ? n - 1 : 0; // 1-based on the wire, 0-based for Scintilla
    }

    // 2. A section named in the input. Longest title first, so "Implementation
    //    Plan" wins over a hypothetical "Plan"; ties go to the earliest
    //    section, keeping the result stable.
    const SpecSection *best = nullptr;
    size_t bestLen = 0;
    for (const SpecSection &sec : sections)
    {
        if (sec.title.empty() || sec.title.size() <= bestLen)
            continue;
        if (toolInput.find(sec.title) != std::string::npos)
        {
            best = &sec;
            bestLen = sec.title.size();
        }
    }
    return best ? (long) best->line : -1;
}

void AgentConversation::addUserTurn(std::string text)
{
    ConvItem it;
    it.kind = ConvKind::UserTurn;
    it.text = std::move(text);
    it.pending = true;
    items_.push_back(std::move(it));
    ++revision_;
}

void AgentConversation::acknowledgePending()
{
    for (size_t i = items_.size(); i-- > 0;)
        if (items_[i].kind == ConvKind::UserTurn)
        {
            if (items_[i].pending)
            {
                items_[i].pending = false;
                ++revision_;
            }
            return;
        }
}

void AgentConversation::addNotice(std::string text)
{
    ConvItem it;
    it.kind = ConvKind::Notice;
    it.text = std::move(text);
    items_.push_back(std::move(it));
    ++revision_;
}

bool AgentConversation::addEvent(const SpecAgentEvent &e)
{
    ConvItem it;
    switch (e.kind)
    {
        case SpecAgentEventKind::AssistantText:
            acknowledgePending();
            it.kind = ConvKind::Assistant;
            it.text = e.text;
            break;
        case SpecAgentEventKind::ToolUse:
            acknowledgePending();
            it.kind = ConvKind::ToolUse;
            it.toolName = e.toolName;
            it.toolInput = e.toolInput;
            it.toolUseId = e.toolUseId;
            break;
        case SpecAgentEventKind::ToolResult:
            it.kind = ConvKind::ToolResult;
            it.text = e.text;
            it.toolUseId = e.toolUseId;
            it.isError = e.isError;
            break;
        case SpecAgentEventKind::Error:
            it.kind = ConvKind::Error;
            it.text = e.text;
            it.isError = true;
            break;
        case SpecAgentEventKind::TurnComplete:
            // The turn's prose already arrived as assistant events; only a
            // failure is worth its own line.
            acknowledgePending();
            if (!e.isError)
                return false;
            it.kind = ConvKind::Error;
            it.text = e.text.empty() ? "The agent turn failed." : e.text;
            it.isError = true;
            break;
        case SpecAgentEventKind::Exited:
            it.kind = ConvKind::Notice;
            it.text = e.exitCode == 0
                    ? "The agent exited."
                    : "The agent exited with code " + std::to_string(e.exitCode) + ".";
            break;
        case SpecAgentEventKind::SessionStart:
        case SpecAgentEventKind::Ignored:
        default:
            return false;
    }
    items_.push_back(std::move(it));
    ++revision_;
    return true;
}

void AgentConversation::clear()
{
    items_.clear();
    lines_.clear();
    laidOutWidth_ = -1;
    ++revision_;
    laidOutRevision_ = ~0UL;
}

const std::vector<ConvLine> &AgentConversation::layout(int width)
{
    if (width == laidOutWidth_ && revision_ == laidOutRevision_)
        return lines_;

    lines_.clear();
    if (width >= 1)
    {
        for (size_t i = 0; i < items_.size(); ++i)
        {
            const ConvItem &item = items_[i];
            // A blank row between items keeps the transcript readable.
            if (!lines_.empty())
                lines_.push_back(ConvLine{"", item.kind, i, false,
                                          item.pending, item.isError});
            auto wrapped = convWrap(convItemText(item), width);
            if (wrapped.empty())
                wrapped.emplace_back();
            for (size_t w = 0; w < wrapped.size(); ++w)
                lines_.push_back(ConvLine{std::move(wrapped[w]), item.kind, i,
                                          w == 0, item.pending, item.isError});
        }
    }
    laidOutWidth_ = width;
    laidOutRevision_ = revision_;
    return lines_;
}

} // namespace turbo
