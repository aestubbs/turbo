#ifndef TURBO_SPECMODEL_H
#define TURBO_SPECMODEL_H

#include <string>
#include <string_view>
#include <vector>

namespace turbo {

// The structured face of a spec document (specs/*.md): a small YAML-ish
// frontmatter block plus conventions read out of the Markdown body. This is
// the data the Spec Manager displays and the implementation gate judges; the
// document itself stays plain Markdown any agent or human can edit.
//
// Lifecycle (frontmatter 'status'):
//   draft -> ready -> reviewed -> implementing -> implemented   (plus parked)
struct SpecInfo
{
    std::string path; // absolute path of the .md file; empty when parsed from text

    // Frontmatter fields. All optional in the file; missing ones stay empty.
    std::string title, status, domain, created, updated, id;
    std::vector<std::string> depends, supersedes;
    bool hasFrontmatter {false};

    // Read from the body:
    int planDone {0};      // "- [x]" checkboxes under "# Implementation Plan"
    int planTotal {0};     // all "- [ ]"/"- [x]" checkboxes under it
    int openQuestions {0}; // "- **Q<n>" bullets under "# Decisions"

    // The name a 'depends' entry refers this spec by: its frontmatter id if
    // set, else the filename stem ("specs/spec-workbench.md" -> "spec-workbench").
    std::string refName() const;
};

// Parse a spec document's full text. 'path' is stored verbatim in the result.
SpecInfo parseSpec(std::string_view text, std::string_view path = {});

// One "# Heading" section of the body, for the Workbench's status strip.
struct SpecSection
{
    std::string title;
    size_t line;        // 0-based line number of the heading
    bool empty;         // no non-blank content before the next heading
};
std::vector<SpecSection> specSections(std::string_view text);

// Return the document with frontmatter 'key' set to 'value' (replacing an
// existing line or inserting one; creates the frontmatter block if absent).
std::string withFrontmatterValue(std::string_view text, std::string_view key,
                                 std::string_view value);

// The implementation gate (FR20): why 'spec' may not be handed to a coding
// agent, given every spec in the project. Empty result = the gate passes.
// Conditions: status is 'reviewed'; every 'depends' target exists and is
// 'implemented'; no open questions.
std::vector<std::string> specGateBlockers(const SpecInfo &spec,
                                          const std::vector<SpecInfo> &all);

// Read every *.md under 'specsDir' (recursively), parsed, sorted by
// frontmatter 'updated' (newest first; undated last), then by path. A missing
// or unreadable directory yields an empty list.
std::vector<SpecInfo> scanSpecsDir(const std::string &specsDir);

// Return the document with 'block' appended at the end of the "# <section>"
// section (kept apart by blank lines; before the next heading). If the
// section is missing it is created at the end of the document.
std::string appendToSpecSection(std::string_view text, std::string_view section,
                                std::string_view block);

// "My Great Feature!" -> "my-great-feature" (filename-safe, D7 naming).
std::string kebabCase(std::string_view s);

// The default spec template (FR3): frontmatter + the canonical headings.
// 'goal' (may be empty) seeds the Objective section; 'date' is YYYY-MM-DD.
std::string specTemplate(std::string_view title, std::string_view domain,
                         std::string_view goal, std::string_view date);

} // namespace turbo

#endif // TURBO_SPECMODEL_H
