#include <turbo/specmodel.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace turbo {

// Split into lines without copying; the trailing line needs no newline.
static std::vector<std::string_view> splitLines(std::string_view text)
{
    std::vector<std::string_view> lines;
    size_t pos = 0;
    while (pos <= text.size())
    {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos)
        {
            lines.push_back(text.substr(pos));
            break;
        }
        std::string_view line = text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        lines.push_back(line);
        pos = nl + 1;
    }
    return lines;
}

static std::string_view trim(std::string_view s)
{
    while (!s.empty() && std::isspace((unsigned char) s.front()))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace((unsigned char) s.back()))
        s.remove_suffix(1);
    return s;
}

// A frontmatter list value: "[a, b]" or "a, b" -> {"a", "b"}.
static std::vector<std::string> parseList(std::string_view v)
{
    v = trim(v);
    if (!v.empty() && v.front() == '[' && v.back() == ']')
    {
        v.remove_prefix(1);
        v.remove_suffix(1);
    }
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= v.size())
    {
        size_t comma = v.find(',', pos);
        std::string_view item = comma == std::string_view::npos
                              ? v.substr(pos) : v.substr(pos, comma - pos);
        item = trim(item);
        if (!item.empty())
            out.emplace_back(item);
        if (comma == std::string_view::npos)
            break;
        pos = comma + 1;
    }
    return out;
}

std::string SpecInfo::refName() const
{
    if (!id.empty())
        return id;
    size_t slash = path.find_last_of("/\\");
    std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot != 0)
        base.resize(dot);
    return base;
}

SpecInfo parseSpec(std::string_view text, std::string_view path)
{
    SpecInfo info;
    info.path = std::string {path};
    auto lines = splitLines(text);

    // Frontmatter: a leading "---" line closed by another "---" line.
    size_t body = 0;
    if (!lines.empty() && trim(lines[0]) == "---")
    {
        for (size_t i = 1; i < lines.size(); ++i)
        {
            if (trim(lines[i]) == "---")
            {
                info.hasFrontmatter = true;
                body = i + 1;
                break;
            }
        }
        if (info.hasFrontmatter)
            for (size_t i = 1; trim(lines[i]) != "---"; ++i)
            {
                std::string_view line = lines[i];
                size_t colon = line.find(':');
                if (colon == std::string_view::npos)
                    continue;
                std::string_view key = trim(line.substr(0, colon));
                std::string_view val = trim(line.substr(colon + 1));
                if      (key == "title")      info.title   = std::string {val};
                else if (key == "status")     info.status  = std::string {val};
                else if (key == "domain")     info.domain  = std::string {val};
                else if (key == "created")    info.created = std::string {val};
                else if (key == "updated")    info.updated = std::string {val};
                else if (key == "id")         info.id      = std::string {val};
                else if (key == "depends")    info.depends    = parseList(val);
                else if (key == "supersedes") info.supersedes = parseList(val);
            }
    }

    // Body conventions: plan checkboxes live under "# Implementation Plan",
    // open-question bullets ("- **Q...") under "# Decisions". Tracking the
    // current section keeps a checkbox in, say, Test Strategy out of Progress.
    std::string_view section;
    for (size_t i = body; i < lines.size(); ++i)
    {
        std::string_view line = lines[i];
        if (line.rfind("# ", 0) == 0)
        {
            section = trim(line.substr(2));
            continue;
        }
        std::string_view t = trim(line);
        if (section == "Implementation Plan")
        {
            if (t.rfind("- [ ]", 0) == 0)
                ++info.planTotal;
            else if (t.rfind("- [x]", 0) == 0 || t.rfind("- [X]", 0) == 0)
            {
                ++info.planTotal;
                ++info.planDone;
            }
        }
        else if (section == "Decisions")
        {
            if (t.rfind("- **Q", 0) == 0)
                ++info.openQuestions;
        }
    }
    return info;
}

std::vector<SpecSection> specSections(std::string_view text)
{
    std::vector<SpecSection> out;
    auto lines = splitLines(text);

    // Skip frontmatter so "---" delimiters aren't mistaken for content.
    size_t start = 0;
    if (!lines.empty() && trim(lines[0]) == "---")
        for (size_t i = 1; i < lines.size(); ++i)
            if (trim(lines[i]) == "---")
            {
                start = i + 1;
                break;
            }

    for (size_t i = start; i < lines.size(); ++i)
    {
        if (lines[i].rfind("# ", 0) != 0)
            continue;
        SpecSection s;
        s.title = std::string {trim(lines[i].substr(2))};
        s.line = i;
        s.empty = true;
        // Any non-blank line before the next heading makes it non-empty.
        for (size_t j = i + 1; j < lines.size() && lines[j].rfind("# ", 0) != 0; ++j)
            if (!trim(lines[j]).empty())
            {
                s.empty = false;
                break;
            }
        out.push_back(std::move(s));
    }
    return out;
}

std::string withFrontmatterValue(std::string_view text, std::string_view key,
                                 std::string_view value)
{
    std::string keyPrefix = std::string {key} + ":";
    auto lines = splitLines(text);
    std::string out;
    out.reserve(text.size() + key.size() + value.size() + 8);

    auto appendLine = [&out] (std::string_view l) {
        out += l;
        out += '\n';
    };

    if (!lines.empty() && trim(lines[0]) == "---")
    {
        bool replaced = false, closed = false;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            std::string_view line = lines[i];
            if (i > 0 && !closed && trim(line) == "---")
            {
                if (!replaced)
                    appendLine(keyPrefix + " " + std::string {value});
                closed = true;
                appendLine(line);
                continue;
            }
            if (!closed && i > 0 && trim(line).rfind(keyPrefix, 0) == 0)
            {
                appendLine(keyPrefix + " " + std::string {value});
                replaced = true;
                continue;
            }
            appendLine(line);
        }
        if (closed)
        {
            // splitLines + appendLine add exactly one newline beyond the
            // original (the final element is "" for trailing-newline text,
            // and a no-newline tail gets one appended): drop it.
            if (!out.empty() && out.back() == '\n')
                out.pop_back();
            return out;
        }
    }
    // No (closed) frontmatter: create one ahead of the existing text.
    std::string created = "---\n";
    created += keyPrefix + " " + std::string {value} + "\n";
    created += "---\n";
    created += text;
    return created;
}

std::vector<std::string> specGateBlockers(const SpecInfo &spec,
                                          const std::vector<SpecInfo> &all)
{
    std::vector<std::string> blockers;
    if (spec.status != "reviewed")
        blockers.push_back(spec.status.empty()
            ? "no status (needs review)"
            : "status is '" + spec.status + "' (needs review)");
    for (const std::string &dep : spec.depends)
    {
        const SpecInfo *found = nullptr;
        for (const SpecInfo &s : all)
            if (s.refName() == dep)
            {
                found = &s;
                break;
            }
        if (!found)
            blockers.push_back("dependency '" + dep + "' not found");
        else if (found->status != "implemented")
            blockers.push_back("dependency '" + dep + "' is '" +
                               (found->status.empty() ? "draft" : found->status) +
                               "', not implemented");
    }
    if (spec.openQuestions > 0)
        blockers.push_back(std::to_string(spec.openQuestions) + " open question" +
                           (spec.openQuestions == 1 ? "" : "s"));
    return blockers;
}

std::vector<SpecInfo> scanSpecsDir(const std::string &specsDir)
{
    std::vector<SpecInfo> specs;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(specsDir, ec), end;
    if (ec)
        return specs;
    for (; it != end; it.increment(ec))
    {
        if (ec)
            break;
        std::error_code ec2;
        if (!it->is_regular_file(ec2))
            continue;
        std::string path = it->path().string();
        if (it->path().extension() != ".md")
            continue;
        std::ifstream f(path, std::ios::binary);
        if (!f)
            continue;
        std::ostringstream ss;
        ss << f.rdbuf();
        specs.push_back(parseSpec(ss.str(), path));
    }
    std::sort(specs.begin(), specs.end(),
              [] (const SpecInfo &a, const SpecInfo &b) {
                  if (a.updated != b.updated)
                  {
                      if (a.updated.empty()) return false; // undated last
                      if (b.updated.empty()) return true;
                      return a.updated > b.updated;        // newest first
                  }
                  return a.path < b.path;
              });
    return specs;
}

std::string appendToSpecSection(std::string_view text, std::string_view section,
                                std::string_view block)
{
    std::string heading = "# " + std::string {section};
    auto lines = splitLines(text);
    // splitLines yields a final "" element when the text ends in a newline;
    // drop it so the rebuild below controls the trailing newline itself.
    if (!lines.empty() && lines.back().empty() && !text.empty() &&
        text.back() == '\n')
        lines.pop_back();

    // Find the section's heading, then the end of its content (the last
    // non-blank line before the next heading or EOF).
    size_t headingAt = lines.size();
    for (size_t i = 0; i < lines.size(); ++i)
        if (trim(lines[i]) == heading)
        {
            headingAt = i;
            break;
        }
    std::string out;
    auto appendLine = [&out] (std::string_view l) {
        out += l;
        out += '\n';
    };
    if (headingAt == lines.size())
    {
        // No such section: create it at the end of the document.
        for (auto l : lines)
            appendLine(l);
        if (!lines.empty())
            appendLine("");
        appendLine(heading);
        appendLine("");
        appendLine(block);
        return out;
    }
    size_t insertAt = headingAt + 1; // after the last non-blank content line
    for (size_t i = headingAt + 1; i < lines.size(); ++i)
    {
        if (lines[i].rfind("# ", 0) == 0)
            break;
        if (!trim(lines[i]).empty())
            insertAt = i + 1;
    }
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (i == insertAt)
        {
            appendLine("");
            appendLine(block);
        }
        appendLine(lines[i]);
    }
    if (insertAt == lines.size())
    {
        appendLine("");
        appendLine(block);
    }
    return out;
}

std::string kebabCase(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    bool pendingDash = false;
    for (char c : s)
    {
        if (std::isalnum((unsigned char) c))
        {
            if (pendingDash && !out.empty())
                out += '-';
            pendingDash = false;
            out += (char) std::tolower((unsigned char) c);
        }
        else
            pendingDash = true;
    }
    return out;
}

std::string specTemplate(std::string_view title, std::string_view domain,
                         std::string_view goal, std::string_view date)
{
    std::string t;
    t += "---\n";
    t += "title: " + std::string {title} + "\n";
    t += "status: draft\n";
    if (!domain.empty())
        t += "domain: " + std::string {domain} + "\n";
    t += "created: " + std::string {date} + "\n";
    t += "updated: " + std::string {date} + "\n";
    t += "---\n";
    t += "\n# Background\n";
    t += "\n# Objective\n";
    if (!goal.empty())
        t += "\n" + std::string {goal} + "\n";
    t += "\n# Non-Goals\n";
    t += "\n# Functional Requirements\n";
    t += "\n# UX Considerations\n";
    t += "\n# Security Considerations\n";
    t += "\n# Auditability and Observability\n";
    t += "\n# Test Strategy\n";
    t += "\n# References\n";
    t += "\n# Decisions\n";
    t += "\n# Implementation Plan\n";
    t += "\n# Summary\n";
    return t;
}

} // namespace turbo
