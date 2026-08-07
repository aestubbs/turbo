#ifndef TURBO_LSPMANAGER_H
#define TURBO_LSPMANAGER_H

struct EditorWindow;

#include <string>
#include <vector>

// A server the user configured: a command line that serves one or more language
// ids. `key` is a stable identifier -- a language id for the per-language
// `lsp.server.<lang>` entries (which override the built-in default for that
// language), or the name for `lsp.extra.<name>` entries (extra servers that may
// serve several languages, e.g. one server serving both "php" and "blade").
// Defined outside the ifdef so the LSP-disabled stub's configure() takes it too.
struct LspServerSpec
{
    std::string key;
    std::string command;                  // full command line (whitespace-split)
    std::vector<std::string> languages;   // language ids this server serves
    bool serves(const std::string &lang) const noexcept
    {
        for (auto &l : languages)
            if (l == lang)
                return true;
        return false;
    }
};

#ifdef TURBO_ENABLE_LSP

#include <turbo/lsp/client.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Owns the language-server clients and bridges Turbo's editors to them.
//
// A server serves one-or-more language ids, and a language may be served by
// MORE THAN ONE server (e.g. a general PHP server plus a Laravel LSP). Clients
// are one process per server, spawned lazily on the first didOpen for a language
// the server handles. Document lifecycle and requests fan out to every server
// serving the document's language; completions are merged and each server's
// diagnostics are tracked separately so they don't clobber one another.
//
// All public methods run on the main thread; pump() is called from the
// application's idle loop to deliver server messages.
class LspManager
{
public:
    LspManager() noexcept;
    ~LspManager();

    // The workspace root (used as the LSP rootUri). Call before opening files.
    void setRootPath(const char *path) noexcept;

    // Apply user configuration: whether LSP is enabled and the user's server
    // specs (per-language overrides and extra multi-language servers). Affects
    // servers started after this call (already-running servers are unchanged).
    void configure(bool enabled, std::vector<LspServerSpec> servers) noexcept;

    // Project-scoped servers (e.g. from project-type auto-detection), running
    // alongside the configured ones. Stops the PREVIOUS project's detected
    // servers first so they don't linger with a stale root. Call on project
    // open (with the new root already set via setRootPath) and with {} on close.
    void setProjectServers(std::vector<LspServerSpec> servers) noexcept;

    // Editor document lifecycle. No-ops for files whose language has no server.
    void didOpen(EditorWindow &w) noexcept;
    void didChange(EditorWindow &w) noexcept;   // marks the document dirty
    void didSave(EditorWindow &w) noexcept;
    void didClose(EditorWindow &w) noexcept;

    // Language features.
    void charAdded(EditorWindow &w, int ch) noexcept;       // (no auto-trigger)
    void requestCompletion(EditorWindow &w) noexcept;       // explicit (Alt-Space)
    void showCompletion(EditorWindow &w) noexcept;          // display the popup (main loop)
    void hover(EditorWindow &w, long pos) noexcept;         // dwell start
    void hoverEnd(EditorWindow &w) noexcept;                // dwell end

    // Flush debounced changes and deliver queued server messages. Cheap to call
    // every idle tick.
    void pump() noexcept;

    // Stop every server (joins reader/writer threads). Call on app shutdown.
    void shutdown() noexcept;

private:
    struct ServerConfig
    {
        std::string key;                     // stable id (language id, or extra name)
        std::string command;
        std::vector<std::string> args;
        std::vector<std::string> languages;  // language ids this server serves
        bool valid() const noexcept { return !command.empty() && !languages.empty(); }
        bool serves(const std::string &lang) const noexcept
        {
            for (auto &l : languages)
                if (l == lang)
                    return true;
            return false;
        }
    };

    struct Diagnostic
    {
        long start {0};     // Scintilla byte positions (at time of publish)
        long end {0};
        int severity {1};   // 1=Error, 2=Warning, 3=Info, 4=Hint
        std::string message;
    };

    struct Document
    {
        std::string uri;
        std::string languageId;
        int version {1};
        // Every server serving this document's language; requests fan out to all.
        std::vector<turbo::lsp::Client *> clients;
        // Diagnostics kept per source server, so one server's publish does not
        // clobber another's; renderAnnotations draws the union.
        std::unordered_map<turbo::lsp::Client *, std::vector<Diagnostic>> diagsBySource;
        std::vector<std::string> pendingCompletions; // merged, awaiting display
    };

    // The effective set of servers for a language: the built-in default for it
    // (unless a same-key user entry overrides it) plus every user server whose
    // languages include it.
    std::vector<ServerConfig> serversForLanguage(const std::string &languageId) noexcept;
    // Lazily start (if needed) and return the live clients serving 'languageId'.
    std::vector<turbo::lsp::Client *> clientsFor(const std::string &languageId) noexcept;
    // Start (if needed) and return the one client for a server config, keyed and
    // negatively-cached by cfg.key; null if it can't be started.
    turbo::lsp::Client *clientForServer(const ServerConfig &cfg) noexcept;
    // Server-specific 'initializationOptions' (e.g. intelephense's storagePath).
    turbo::lsp::Json initOptionsFor(const ServerConfig &cfg) noexcept;
    void onServerMessage(turbo::lsp::Client *source, const turbo::lsp::Json &msg) noexcept;
    void flushChange(EditorWindow &w) noexcept;
    EditorWindow *findByUri(const std::string &uri) noexcept;
    // Replace 'source's diagnostics for this document, then redraw the union.
    void applyDiagnostics(EditorWindow &w, turbo::lsp::Client *source,
                          const turbo::lsp::Json &diagnostics,
                          turbo::lsp::PositionEncoding enc) noexcept;
    // Draws the stored diagnostics (across all sources) as annotations (a '~~~~'
    // run plus the message on the line below each diagnostic's span).
    void renderAnnotations(EditorWindow &w) noexcept;
    // Builds the textDocument/position params for the caret, in 'client's encoding.
    turbo::lsp::Json positionParams(EditorWindow &w, long pos,
                                    turbo::lsp::Client *client) noexcept;
    void sendCompletion(EditorWindow &w) noexcept;
    Document *docFor(EditorWindow &w) noexcept;

    std::string rootUri;
    bool enabled {true};
    std::vector<LspServerSpec> userServers;      // per-language overrides + extras
    std::vector<LspServerSpec> projectServers;   // auto-detected, project-scoped
    std::unordered_map<std::string, std::unique_ptr<turbo::lsp::Client>> clients; // serverKey -> client
    std::unordered_set<std::string> deadServers; // serverKey: unavailable/failed
    std::unordered_map<EditorWindow *, Document> docs;
    std::unordered_set<EditorWindow *> dirty;
};

#else // !TURBO_ENABLE_LSP

#include <string>
#include <utility>
#include <vector>

// Stub so the rest of the app compiles unchanged when LSP is disabled.
class LspManager
{
public:
    void setRootPath(const char *) noexcept {}
    void configure(bool, std::vector<LspServerSpec>) noexcept {}
    void setProjectServers(std::vector<LspServerSpec>) noexcept {}
    void didOpen(EditorWindow &) noexcept {}
    void didChange(EditorWindow &) noexcept {}
    void didSave(EditorWindow &) noexcept {}
    void didClose(EditorWindow &) noexcept {}
    void charAdded(EditorWindow &, int) noexcept {}
    void requestCompletion(EditorWindow &) noexcept {}
    void showCompletion(EditorWindow &) noexcept {}
    void hover(EditorWindow &, long) noexcept {}
    void hoverEnd(EditorWindow &) noexcept {}
    void pump() noexcept {}
    void shutdown() noexcept {}
};

#endif // TURBO_ENABLE_LSP

#endif // TURBO_LSPMANAGER_H
