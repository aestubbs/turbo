#include "specagentsession.h"

#include <vector>

SpecAgentSession::~SpecAgentSession()
{
    stop();
}

bool SpecAgentSession::start(const std::string &command, const std::string &cwd,
                             const std::string &resumeSessionId) noexcept
{
    stop();

    std::string prog = turbo::specAgentProgram(command);
    if (prog.empty())
        return false;
    // argv only ever carries flags -- the prompt goes over stdin as JSON.
    auto args = turbo::specAgentArgv(command, resumeSessionId);
    if (!proc.start(prog, args, cwd, {}))
        return false;

    eof_.store(false);
    reaped_ = false;
    running_ = true;
    busy_ = false;
    lineBuf.clear();
    if (resumeSessionId.empty())
    {
        sessionId_.clear();
        model_.clear();
    }
    else
        sessionId_ = resumeSessionId;
    { std::lock_guard<std::mutex> lk(mx); incoming.clear(); }

    reader = std::thread([this] {
        char buf[8192];
        for (;;)
        {
            long n = proc.readStdout(buf, sizeof buf);
            if (n <= 0)
                break; // EOF or error
            {
                std::lock_guard<std::mutex> lk(mx);
                incoming.append(buf, (size_t) n);
            }
            if (onWake)
                onWake();
        }
        eof_.store(true);
        if (onWake)
            onWake();
    });
    return true;
}

bool SpecAgentSession::send(const std::string &text) noexcept
{
    if (!running_ || text.empty())
        return false;
    std::string line = turbo::specAgentTurnJson(text);
    line += '\n';
    if (!proc.writeStdin(line.data(), line.size()))
        return false;
    busy_ = true;
    return true;
}

void SpecAgentSession::pump() noexcept
{
    if (!running_)
        return;

    std::string data;
    {
        std::lock_guard<std::mutex> lk(mx);
        if (!incoming.empty())
            data.swap(incoming);
    }
    if (!data.empty())
    {
        lineBuf += data;
        size_t pos;
        std::vector<turbo::SpecAgentEvent> events;
        while ((pos = lineBuf.find('\n')) != std::string::npos)
        {
            std::string line = lineBuf.substr(0, pos);
            lineBuf.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            events.clear();
            turbo::parseSpecAgentLine(line, events);
            for (auto &e : events)
            {
                if (e.kind == turbo::SpecAgentEventKind::Ignored)
                    continue;
                if (e.kind == turbo::SpecAgentEventKind::SessionStart)
                {
                    // The agent assigns the id; hold it so the conversation
                    // can be resumed after the window closes.
                    if (!e.sessionId.empty())
                        sessionId_ = e.sessionId;
                    if (!e.model.empty())
                        model_ = e.model;
                }
                else if (e.kind == turbo::SpecAgentEventKind::TurnComplete)
                    busy_ = false;
                if (onEvent)
                    onEvent(e);
            }
        }
    }

    if (eof_.load() && !reaped_)
    {
        if (reader.joinable())
            reader.join();
        int code = proc.wait(); // reader is done; safe to reap here
        reaped_ = true;
        running_ = false;
        busy_ = false;
        turbo::SpecAgentEvent e;
        e.kind = turbo::SpecAgentEventKind::Exited;
        e.exitCode = code;
        e.sessionId = sessionId_;
        if (onEvent)
            onEvent(e);
    }
}

void SpecAgentSession::stop() noexcept
{
    if (reader.joinable())
    {
        proc.terminate(); // the reader then sees EOF and exits
        reader.join();
    }
    reaped_ = true;
    running_ = false;
    busy_ = false;
    lineBuf.clear();
    {
        std::lock_guard<std::mutex> lk(mx);
        incoming.clear();
    }
}
