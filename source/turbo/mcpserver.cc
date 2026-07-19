// McpServer — see mcpserver.h. Implements a minimal MCP server: initialize,
// tools/list, tools/call, ping. Includes the large nlohmann/json header, so it
// is kept out of the unity batch (CMakeLists.txt), like buildconfig.cc.

#include "mcpserver.h"
#include "luamanager.h"

#include <turbo/process.h>

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
#include <cstdlib>
#elif defined(__linux__)
#include <unistd.h>
#endif

using Json = nlohmann::json;

namespace {

// The MCP protocol revision we implement. We echo the client's requested
// version when it sends one; this is the fallback.
const char *kProtocolVersion = "2024-11-05";

// A JSON-RPC error reply.
Json rpcError(const Json &id, int code, const std::string &msg)
{
    return Json{{"jsonrpc", "2.0"}, {"id", id},
                {"error", {{"code", code}, {"message", msg}}}};
}

// A JSON-RPC success reply.
Json rpcResult(const Json &id, Json result)
{
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

// A tools/call result carrying a single text block.
Json textContent(const std::string &text, bool isError = false)
{
    return Json{{"content", Json::array({Json{{"type", "text"}, {"text", text}}})},
                {"isError", isError}};
}

// Build a JSON-Schema object from a list of {name, jsonType, description, required}.
struct Param { const char *name; const char *type; const char *desc; bool required; };
Json objectSchema(std::initializer_list<Param> params)
{
    Json props = Json::object();
    Json required = Json::array();
    for (const auto &p : params)
    {
        props[p.name] = {{"type", p.type}, {"description", p.desc}};
        if (p.required)
            required.push_back(p.name);
    }
    Json schema = {{"type", "object"}, {"properties", props}};
    if (!required.empty())
        schema["required"] = required;
    return schema;
}

// The built-in editor tools and their input schemas (mirrors the turbo.* API).
Json builtinToolList()
{
    Json tools = Json::array();
    auto add = [&](const char *name, const char *desc, Json schema) {
        tools.push_back({{"name", name}, {"description", desc}, {"inputSchema", std::move(schema)}});
    };
    add("open_file", "Open (or focus) a file in the editor by absolute path.",
        objectSchema({{"path", "string", "Absolute path of the file to open", true}}));
    add("active_file", "Return the path of the focused editor, or \"\" if none.",
        objectSchema({}));
    add("file_text", "Return the full text of the focused editor.",
        objectSchema({}));
    add("insert_text", "Insert text at the focused editor's cursor.",
        objectSchema({{"text", "string", "Text to insert", true}}));
    add("save", "Save the focused editor.", objectSchema({}));
    add("run_command", "Dispatch a turboIDE command by its numeric id (advanced).",
        objectSchema({{"command", "integer", "The cmXxx command id", true}}));
    add("project_root", "Return the absolute path of the open project.",
        objectSchema({}));
    add("shell", "Run a shell command in the project directory; returns its stdout.",
        objectSchema({{"cmd", "string", "The shell command line", true}}));
    // ask_user has a nested schema the flat objectSchema helper can't express.
    Json questionSchema = {
        {"type", "object"},
        {"properties", {
            {"prompt", {{"type", "string"},
                        {"description", "The question text"}}},
            {"options", {{"type", "array"}, {"items", {{"type", "string"}}},
                         {"description", "Preset choices (max 16). Omit for a "
                                         "free-text answer."}}},
            {"multi_select", {{"type", "boolean"},
                              {"description", "Allow choosing several options"}}},
            {"free_text", {{"type", "boolean"},
                           {"description", "Offer a free-text field besides "
                                           "the options"}}},
        }},
        {"required", Json::array({"prompt"})},
    };
    add("ask_user",
        "Ask the user questions through a native turboIDE dialog wizard (one "
        "page per question, Back/Next/Finish). Use it for decisions with "
        "enumerable options or short structured input; keep open-ended "
        "discussion in the conversation. Returns JSON {cancelled, answers: "
        "[{prompt, selected, text}]}. cancelled=true means the user dismissed "
        "the wizard - never treat that as empty answers.",
        Json{{"type", "object"},
             {"properties", {{"questions", {{"type", "array"},
                                            {"items", questionSchema},
                                            {"description", "The questions, in "
                                                            "order (max 8)"}}}}},
             {"required", Json::array({"questions"})}});
    return tools;
}

// Map a Lua command name to an MCP tool name: "lua_" + lowercased, with any
// character outside [A-Za-z0-9_-] replaced by '_' (MCP tool-name charset).
std::string luaToolName(const std::string &raw)
{
    std::string s = "lua_";
    for (char c : raw)
    {
        unsigned char u = (unsigned char) c;
        if (std::isalnum(u) || c == '_' || c == '-')
            s += (char) std::tolower(u);
        else
            s += '_';
    }
    return s;
}

// JSON Schema for a Lua command's declared params (empty -> an empty object).
Json luaToolSchema(const std::vector<LuaParam> &params)
{
    Json props = Json::object();
    Json required = Json::array();
    for (const auto &p : params)
    {
        // Normalise the declared type to a JSON-schema type name.
        std::string t = p.type;
        if (t != "string" && t != "number" && t != "integer" && t != "boolean")
            t = "string";
        props[p.name] = {{"type", t}, {"description", p.description}};
        if (p.required)
            required.push_back(p.name);
    }
    Json schema = {{"type", "object"}, {"properties", props}};
    if (!required.empty())
        schema["required"] = required;
    return schema;
}

} // namespace

McpServer::McpServer(LuaManager &aLua,
                     std::function<void(const std::string &)> aMessage) noexcept
    : lua(aLua), message(std::move(aMessage))
{
}

McpServer::~McpServer()
{
    stop();
}

bool McpServer::start(const std::string &socketPath) noexcept
{
    if (transport.running())
        return true;
    bool ok = transport.start(socketPath);
    if (!ok && message)
        message("MCP: could not start server on " + socketPath);
    return ok;
}

void McpServer::stop() noexcept
{
    transport.stop();
}

void McpServer::pump() noexcept
{
    if (inPump)
        return; // a tool triggered nested idle(); don't re-enter dispatch
    inPump = true;
    transport.pump([this](uint64_t connId, const std::string &msg) {
        handleMessage(connId, msg);
    });
    inPump = false;
}

void McpServer::handleMessage(uint64_t connId, const std::string &msg) noexcept
{
    Json req = Json::parse(msg, nullptr, /*allow_exceptions=*/false);
    if (req.is_discarded() || !req.is_object())
    {
        transport.send(connId, rpcError(nullptr, -32700, "parse error").dump());
        return;
    }

    Json id = req.contains("id") ? req["id"] : Json(nullptr);
    std::string method = req.value("method", std::string());
    bool isNotification = !req.contains("id");

    if (method == "initialize")
    {
        std::string ver = kProtocolVersion;
        if (req.contains("params") && req["params"].contains("protocolVersion") &&
            req["params"]["protocolVersion"].is_string())
            ver = req["params"]["protocolVersion"].get<std::string>();
        // Remember who this is: ask_user dialogs are attributed to the
        // asking client so agent-driven UI is never mistaken for turbo's.
        if (req.contains("params") && req["params"].contains("clientInfo") &&
            req["params"]["clientInfo"].is_object())
            clientNames[connId] =
                req["params"]["clientInfo"].value("name", std::string());
        Json result = {
            {"protocolVersion", ver},
            {"capabilities", {{"tools", {{"listChanged", false}}}}},
            {"serverInfo", {{"name", "turboIDE"}, {"version", "0.1"}}},
        };
        transport.send(connId, rpcResult(id, result).dump());
        return;
    }

    if (method == "notifications/initialized" || isNotification)
        return; // notifications carry no id and get no reply

    if (method == "ping")
    {
        transport.send(connId, rpcResult(id, Json::object()).dump());
        return;
    }

    if (method == "tools/list")
    {
        Json tools = builtinToolList();
        for (int i = 0; i < lua.commandCount(); ++i)
            tools.push_back({{"name", luaToolName(lua.commandName(i))},
                             {"description", lua.commandDescription(i)},
                             {"inputSchema", luaToolSchema(lua.commandParams(i))}});
        transport.send(connId, rpcResult(id, {{"tools", tools}}).dump());
        return;
    }

    if (method == "tools/call")
    {
        Json params = req.value("params", Json::object());
        std::string name = params.value("name", std::string());
        Json args = params.contains("arguments") && params["arguments"].is_object()
                        ? params["arguments"] : Json::object();

        const LuaHost &h = lua.hostHooks();
        std::string out;
        bool handled = true;

        if (name == "open_file") { if (h.openFile) h.openFile(args.value("path", std::string())); out = "ok"; }
        else if (name == "active_file") { out = h.activeFilePath ? h.activeFilePath() : ""; }
        else if (name == "file_text") { out = h.activeFileText ? h.activeFileText() : ""; }
        else if (name == "insert_text") { if (h.insertText) h.insertText(args.value("text", std::string())); out = "ok"; }
        else if (name == "save") { if (h.saveFile) h.saveFile(); out = "ok"; }
        else if (name == "run_command") { if (h.runCommand) h.runCommand(args.value("command", 0)); out = "ok"; }
        else if (name == "project_root") { out = h.projectRoot ? h.projectRoot() : ""; }
        else if (name == "shell") { out = h.shell ? h.shell(args.value("cmd", std::string())) : ""; }
        else if (name == "ask_user")
        {
            std::vector<AskQuestion> questions;
            if (args.contains("questions") && args["questions"].is_array())
                for (const Json &jq : args["questions"])
                {
                    if (!jq.is_object() || questions.size() >= 8)
                        continue;
                    AskQuestion q;
                    q.prompt = jq.value("prompt", std::string());
                    if (jq.contains("options") && jq["options"].is_array())
                        for (const Json &opt : jq["options"])
                            if (opt.is_string() && q.options.size() < 16)
                                q.options.push_back(opt.get<std::string>());
                    q.multiSelect = jq.value("multi_select", false);
                    q.freeText = jq.value("free_text", false);
                    if (!q.prompt.empty())
                        questions.push_back(std::move(q));
                }
            if (questions.empty())
            {
                transport.send(connId, rpcResult(id,
                    textContent("ask_user needs at least one question with a "
                                "prompt", /*isError=*/true)).dump());
                return;
            }
            if (!askUser)
            {
                transport.send(connId, rpcResult(id,
                    textContent("ask_user: no UI available",
                                /*isError=*/true)).dump());
                return;
            }
            std::string who = clientNames.count(connId) &&
                              !clientNames[connId].empty()
                            ? clientNames[connId] : "agent";
            std::vector<AskAnswer> answers;
            bool finished = askUser(who, questions, answers);
            Json resp;
            resp["cancelled"] = !finished;
            if (finished)
            {
                Json arr = Json::array();
                for (size_t i = 0; i < questions.size(); ++i)
                {
                    Json a = {{"prompt", questions[i].prompt},
                              {"selected", answers[i].selected},
                              {"text", answers[i].text}};
                    arr.push_back(std::move(a));
                }
                resp["answers"] = std::move(arr);
            }
            out = resp.dump();
        }
        else
            handled = false;

        if (handled)
        {
            transport.send(connId, rpcResult(id, textContent(out)).dump());
            return;
        }

        // A Lua-registered command? Resolve by sanitized tool name.
        for (int i = 0; i < lua.commandCount(); ++i)
            if (luaToolName(lua.commandName(i)) == name)
            {
                std::string result;
                if (lua.runRegisteredCommandJson(i, args.dump(), result))
                    transport.send(connId, rpcResult(id, textContent(result.empty() ? "ok" : result)).dump());
                else
                    transport.send(connId, rpcResult(id, textContent(lua.lastError(), /*isError=*/true)).dump());
                return;
            }

        transport.send(connId, rpcResult(id, textContent("unknown tool: " + name, /*isError=*/true)).dump());
        return;
    }

    transport.send(connId, rpcError(id, -32601, "method not found: " + method).dump());
}

namespace {

// Absolute path of the running turboIDE binary, so the agent launches the same
// build for the `mcp` bridge.
std::string currentExecutablePath()
{
#if defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t sz = sizeof buf;
    if (_NSGetExecutablePath(buf, &sz) == 0)
    {
        char real[PATH_MAX];
        if (::realpath(buf, real))
            return real;
        return buf;
    }
#elif defined(__linux__)
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0)
    {
        buf[n] = '\0';
        return buf;
    }
#endif
    return "turboIDE"; // fall back to PATH lookup
}

// Ensure 'entry' is a line in <projectRoot>/.gitignore, but only when the root
// is a git working tree. Appends it if missing; never duplicates.
void ensureGitignored(const std::string &projectRoot, const std::string &entry) noexcept
{
    std::error_code ec;
    if (!std::filesystem::exists(projectRoot + "/.git", ec))
        return; // not a git repo: leave the tree alone
    std::string path = projectRoot + "/.gitignore";
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line == entry || line == "/" + entry)
                return; // already ignored
        }
    }
    std::ofstream out(path, std::ios::app);
    if (out)
        out << entry << "\n";
}

} // namespace

std::string mcpSocketPath(const std::string &projectRoot) noexcept
{
    std::string p = projectRoot + "/.turbo/mcp.sock";
    // AF_UNIX sun_path is ~104 (macOS) / 108 (Linux). For a very deep project
    // path, fall back to a short name under $TMPDIR (the real path is written
    // into .mcp.json so the bridge still finds it).
    if (p.size() < 100)
    {
        // bind() needs the parent directory to exist; on a project that has
        // never written .turbo (fresh checkout, first open) it doesn't, and
        // the MCP server would silently fail to start.
        std::error_code ec;
        std::filesystem::create_directories(projectRoot + "/.turbo", ec);
        return p;
    }
    std::hash<std::string> h;
    char name[64];
    std::snprintf(name, sizeof name, "/turboide-%zx.sock", (size_t) h(projectRoot));
    const char *tmp = ::getenv("TMPDIR");
    if (!tmp || !*tmp)
        tmp = "/tmp";
    return std::string(tmp) + name;
}

void writeAgentMcpConfig(const std::string &projectRoot,
                         const std::string &socketPath) noexcept
{
    if (projectRoot.empty())
        return;
    std::string path = projectRoot + "/.mcp.json";

    Json root = Json::object();
    {
        std::ifstream in(path);
        if (in)
        {
            Json existing = Json::parse(in, nullptr, /*allow_exceptions=*/false);
            if (!existing.is_discarded() && existing.is_object())
                root = std::move(existing);
        }
    }
    if (!root.contains("mcpServers") || !root["mcpServers"].is_object())
        root["mcpServers"] = Json::object();

    // Set only our entry, preserving any other servers the user configured.
    root["mcpServers"]["turboide"] = {
        {"type", "stdio"},
        {"command", currentExecutablePath()},
        {"args", Json::array({"mcp", "--socket", socketPath})},
    };

    std::ofstream out(path);
    if (out)
        out << root.dump(2) << "\n";

    // .mcp.json holds machine-specific absolute paths; keep it out of git.
    ensureGitignored(projectRoot, ".mcp.json");
}

void registerClaudeMcpServerAsync(const std::string &projectRoot,
                                  const std::string &socketPath) noexcept
{
    if (projectRoot.empty())
        return;
    Json j = {
        {"type", "stdio"},
        {"command", currentExecutablePath()},
        {"args", Json::array({"mcp", "--socket", socketPath})},
    };
    std::string js = j.dump();
    // Detached so the ~1s `claude` CLI calls never block project open. Captures
    // are by value; it touches no turbo state. Re-registers each time so the
    // stored config (socket/exe path) stays current.
    std::thread([projectRoot, js]() {
        std::string out;
        turbo::Process::runToEnd("claude",
            {"mcp", "remove", "turboide", "-s", "local"},
            out, projectRoot, {}, /*mergeStderr=*/true); // ignore "not found"
        turbo::Process::runToEnd("claude",
            {"mcp", "add-json", "turboide", js, "-s", "local"},
            out, projectRoot, {}, /*mergeStderr=*/true);
    }).detach();
}
