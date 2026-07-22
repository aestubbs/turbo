#ifndef TURBO_SPECAGENTSESSION_H
#define TURBO_SPECAGENTSESSION_H

#include <turbo/process.h>
#include <turbo/specagentproto.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

// A live conversation with a headless coding agent: one long-lived child
// process, user turns written to its stdin as stream-json, typed events read
// back from its stdout.
//
// Threading follows the pattern every other turbo transport uses (lsp::Client,
// dap::Client, CommandRunner): a dedicated reader thread does the blocking
// reads and parks raw bytes behind a mutex; the main loop drains them in
// pump() and dispatches onEvent there, so callbacks may safely touch the UI.
// All turbo::Process calls other than readStdout happen on the main thread.
//
// One session per Spec Workbench window -- unlike the single app-global agent
// terminal it replaces, several may run at once.
class SpecAgentSession
{
public:
    // Called on the main thread from pump(), once per parsed event.
    std::function<void(const turbo::SpecAgentEvent &)> onEvent;
    // Called from the reader thread when bytes arrive; wire to
    // TEventQueue::wakeUp() so the idle loop drains promptly instead of
    // waiting for the next input event.
    std::function<void()> onWake;

    SpecAgentSession() = default;
    ~SpecAgentSession();
    SpecAgentSession(const SpecAgentSession &) = delete;
    SpecAgentSession &operator=(const SpecAgentSession &) = delete;

    // Spawn the agent. 'command' is the configured agent command line
    // ("claude", or a user command); 'cwd' is the project root so the agent's
    // own session store and any .mcp.json are found. A non-empty
    // 'resumeSessionId' reattaches to a prior conversation.
    // Any previous child is stopped first. Returns false if it won't spawn.
    bool start(const std::string &command, const std::string &cwd,
               const std::string &resumeSessionId = {}) noexcept;

    // Queue one user turn. Returns false if no child is running or the write
    // fails. Safe to call while a turn is in flight -- the agent processes
    // queued turns in order.
    bool send(const std::string &text) noexcept;

    // Drain buffered output, parse it, and fire onEvent. Call each idle.
    void pump() noexcept;

    // Terminate the child and join the reader. No Exited event is fired.
    void stop() noexcept;

    bool running() const noexcept { return running_; }
    // True between sending a turn and its TurnComplete event.
    bool busy() const noexcept { return busy_; }
    // The agent's own session id, captured from the stream. Empty until the
    // first SessionStart arrives; pass to start() later to resume.
    const std::string &sessionId() const noexcept { return sessionId_; }
    const std::string &model() const noexcept { return model_; }

private:
    turbo::Process proc;
    std::thread reader;
    std::mutex mx;
    std::string incoming;  // reader thread -> main thread (guarded by mx)
    std::string lineBuf;   // main-thread accumulator for partial lines
    std::string sessionId_;
    std::string model_;
    std::atomic<bool> eof_ {false};
    bool reaped_ {true};
    bool running_ {false};
    bool busy_ {false};
};

#endif // TURBO_SPECAGENTSESSION_H
