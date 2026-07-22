#ifndef TURBO_SPECAGENT_H
#define TURBO_SPECAGENT_H

#include <string>

// Agent briefs for spec work (specs/spec-workbench.md FR7/FR8/FR13/FR18).
// A brief = the resolved domain pack (interview questions + readiness
// rubric) wrapped in a mode-specific mission and the write-back contract.
// The brief is delivered as the opening structured turn of the Workbench
// session (spec-agent-integration FR8) -- no brief file, no shell quoting.

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

#endif // TURBO_SPECAGENT_H
