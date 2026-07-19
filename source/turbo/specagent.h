#ifndef TURBO_SPECAGENT_H
#define TURBO_SPECAGENT_H

#include <string>

// Agent briefs for spec work (specs/spec-workbench.md FR7/FR8/FR13/FR18).
// A brief = the resolved domain pack (interview questions + readiness
// rubric) wrapped in a mode-specific mission and the write-back contract.
// The brief is written to a file and the agent CLI is launched with a short
// pointer prompt, sidestepping shell-quoting a page of instructions.

enum class SpecAgentMode { Discuss, Draft, Implement };

const char *specAgentModeName(SpecAgentMode mode) noexcept;

// Resolve the domain pack for 'domain': <projectRoot>/turbo-scripts/
// spec-packs/<domain>.md, then ~/.turbo/spec-packs/<domain>.md, then the
// built-in default pack (D6). An empty domain resolves as "default".
std::string resolveSpecPack(const std::string &domain,
                            const std::string &projectRoot);

// The full brief for a mode, pack included.
std::string specAgentBrief(SpecAgentMode mode, const std::string &specPath,
                           const std::string &domain,
                           const std::string &projectRoot);

// Write 'brief' to <projectRoot>/.turbo/spec-sessions/<stem>-<mode>.md and
// return the path ("" on failure). Overwrites the previous brief for the
// same spec+mode; the durable record is the spec itself (transcripts are
// cache, per the spec's Auditability section).
std::string writeSpecBrief(const std::string &projectRoot,
                           const std::string &specStem,
                           SpecAgentMode mode, const std::string &brief);

// POSIX shell single-quoting for one argument.
std::string shellQuoteArg(const std::string &s);

#endif // TURBO_SPECAGENT_H
