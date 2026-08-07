#ifndef TURBO_SETTINGS_H
#define TURBO_SETTINGS_H

#include <map>
#include <string>
#include <vector>

// A language-server command configured for a given language id (e.g.
// {"cpp", "clangd"} or {"python", "pyright-langserver --stdio"}). This is the
// per-language "primary" server; it overrides the built-in default.
struct LspServerConfig
{
    std::string language;
    std::string command;
};

// An extra language server that runs ALONGSIDE the per-language servers and may
// serve several languages at once (e.g. one server serving both "php" and
// "blade"). Configured via `lsp.extra.<name>.command` / `lsp.extra.<name>.langs`.
struct LspExtraServer
{
    std::string name;
    std::string command;
    std::vector<std::string> languages;
};

// Application-wide settings, persisted to a config file in the user's home
// directory (~/.turborc) between runs.
struct AppSettings
{
    bool autoSaveOnFocusLoss {true};
    bool lspEnabled {true};
    bool showHidden {false}; // include dotfiles/dot-dirs in the file tree
    // File-tree glyph set: "auto" (the default), "nerd", "unicode" or "ascii".
    // "auto" uses Nerd Font pictograms only on terminals known to bundle a Nerd
    // Font fallback, since glyph coverage cannot be probed at runtime and a wrong
    // guess renders blank boxes. See treeicons.h.
    std::string treeIcons {"auto"};
    // Terminal colour depth: "auto" (detect), "full" (always the 24-bit RGB
    // themes) or "16" (the classic 16-colour BIOS fallback, for terminals that
    // render truecolor poorly such as the Windows console). Persisted as
    // "theme.colors". See theme.cc (useClassicColors) and main.cc (which forces
    // Turbo Vision's colour depth from this before the terminal is probed).
    std::string colorMode {"auto"};
    // Command line for the terminal window's shell (e.g. "bash", "zsh -l").
    // Empty = use $SHELL, falling back to /bin/sh.
    std::string terminalShell;
    // Global default coding agent for the agent window, as a preset name
    // ("claude", "codex", "opencode") or a raw command line. A project may
    // override this in its .turbo/config.json. Empty = built-in default.
    std::string defaultAgent;
    // Language-server command overrides. Empty by default; the LSP manager
    // falls back to built-in defaults for languages not listed here.
    std::vector<LspServerConfig> lspServers;
    // Extra servers (each serving one or more languages) that run alongside the
    // per-language servers. Empty by default. See LspExtraServer.
    std::vector<LspExtraServer> lspExtraServers;
    // Colour-theme overrides, stored as "<item>.<fg|bg|style>" -> value (e.g.
    // "sKeyword1.fg" -> "569CD6", "sComment.style" -> "italic"). Persisted under
    // the "theme." prefix in ~/.turborc. The mapping to/from the active colour
    // schemes lives in theme.cc; empty means "use the built-in defaults".
    std::map<std::string, std::string> theme;

    // Returns the configured command for 'language', or "" if none.
    std::string lspCommandFor(const std::string &language) const noexcept;
    // Sets/replaces the command for 'language' (empty command removes it).
    void setLspCommand(const std::string &language, const std::string &command) noexcept;
};

// Read settings from the config file into 's'. Missing file/keys leave the
// corresponding defaults untouched.
void loadSettings(AppSettings &s) noexcept;

// Write 's' to the config file.
void saveSettings(const AppSettings &s) noexcept;

// True if the terminal advertises 24-bit colour via COLORTERM (truecolor/24bit).
// Used to decide the Windows "auto" colour default: modern terminals that
// render truecolor well (Windows Terminal, VS Code, ...) set this, while
// cmd.exe/PowerShell on the classic console do not.
bool terminalAdvertisesTrueColor() noexcept;

// Load the persisted colour-depth preference and, if the effective mode is the
// 16-colour fallback, export TVISION_COLORS so Turbo Vision honours it. MUST be
// called before the TApplication/screen is constructed (Turbo Vision probes the
// terminal's colour support during start-up). On Windows, "auto" resolves to the
// fallback unless the terminal advertises truecolor.
void applyColorDepthPreference() noexcept;

#endif // TURBO_SETTINGS_H
