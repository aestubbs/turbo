#ifndef TURBO_ASKTYPES_H
#define TURBO_ASKTYPES_H

#include <string>
#include <vector>

// The ask_user MCP tool's data model (specs/spec-workbench.md FR11/FR12),
// shared between the MCP server (which parses/serializes JSON) and the
// wizard dialog (which knows nothing of JSON).

struct AskQuestion
{
    std::string prompt;
    std::vector<std::string> options; // preset choices; empty = free text only
    bool multiSelect {false};         // several options may be chosen
    bool freeText {false};            // offer a text field besides the options
};

struct AskAnswer
{
    std::vector<std::string> selected; // chosen options (verbatim strings)
    std::string text;                  // the free-text field, if any
};

#endif // TURBO_ASKTYPES_H
