#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static std::string settingsPath() noexcept
{
    const char *home = getenv("HOME");
    if (!home || !home[0])
        home = getenv("USERPROFILE"); // Windows fallback.
    if (!home || !home[0])
        return {};
    return std::string(home) + "/.turborc";
}

std::string AppSettings::lspCommandFor(const std::string &language) const noexcept
{
    for (auto &s : lspServers)
        if (s.language == language)
            return s.command;
    return {};
}

void AppSettings::setLspCommand(const std::string &language, const std::string &command) noexcept
{
    for (auto it = lspServers.begin(); it != lspServers.end(); ++it)
        if (it->language == language)
        {
            if (command.empty())
                lspServers.erase(it);
            else
                it->command = command;
            return;
        }
    if (!command.empty())
        lspServers.push_back({language, command});
}

// Strips a trailing newline (and CR) in place.
static void chomp(char *s) noexcept
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = '\0';
}

// Splits a comma/space-separated list (e.g. "php, blade") into tokens.
static std::vector<std::string> splitList(const char *s) noexcept
{
    std::vector<std::string> out;
    std::string cur;
    for (; *s; ++s)
    {
        char c = *s;
        if (c == ',' || c == ' ' || c == '\t')
        {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        }
        else
            cur += c;
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

// Finds or appends the extra server named 'name'.
static LspExtraServer &extraServer(AppSettings &s, const std::string &name) noexcept
{
    for (auto &e : s.lspExtraServers)
        if (e.name == name)
            return e;
    s.lspExtraServers.push_back({name, {}, {}});
    return s.lspExtraServers.back();
}

void loadSettings(AppSettings &s) noexcept
{
    auto path = settingsPath();
    if (path.empty())
        return;
    FILE *f = fopen(path.c_str(), "r");
    if (!f)
        return;
    char line[1024];
    const char serverPrefix[] = "lsp.server.";
    const char themePrefix[] = "theme.";
    while (fgets(line, sizeof line, f))
    {
        int v;
        if (sscanf(line, "autosave=%d", &v) == 1)
            s.autoSaveOnFocusLoss = (v != 0);
        else if (sscanf(line, "lsp.enabled=%d", &v) == 1)
            s.lspEnabled = (v != 0);
        else if (sscanf(line, "showhidden=%d", &v) == 1)
            s.showHidden = (v != 0);
        else if (strncmp(line, "terminal.shell=", 15) == 0)
        {
            char *cmd = line + 15;
            chomp(cmd);
            s.terminalShell = cmd;
        }
        else if (strncmp(line, "agent.default=", 14) == 0)
        {
            char *cmd = line + 14;
            chomp(cmd);
            s.defaultAgent = cmd;
        }
        else if (strncmp(line, "tree.icons=", 11) == 0)
        {
            char *val = line + 11;
            chomp(val);
            s.treeIcons = val;
        }
        else if (strncmp(line, "theme.colors=", 13) == 0)
        {
            // Handled before the generic "theme." branch below (which would
            // otherwise stuff it into the theme-override map).
            char *val = line + 13;
            chomp(val);
            s.colorMode = val;
        }
        else if (strncmp(line, themePrefix, sizeof themePrefix - 1) == 0)
        {
            // theme.<item>.<fg|bg|style>=<value>
            char *rest = line + sizeof themePrefix - 1;
            char *eq = strchr(rest, '=');
            if (eq)
            {
                *eq = '\0';
                char *val = eq + 1;
                chomp(val);
                if (rest[0])
                    s.theme[rest] = val;
            }
        }
        else if (strncmp(line, "lsp.extra.", 10) == 0)
        {
            // lsp.extra.<name>.command=<cmdline>  |  lsp.extra.<name>.langs=<csv>
            char *rest = line + 10;
            char *eq = strchr(rest, '=');
            if (eq)
            {
                *eq = '\0';
                char *val = eq + 1;
                chomp(val);
                char *dot = strrchr(rest, '.'); // split "<name>.<field>"
                if (dot && dot != rest)
                {
                    *dot = '\0';
                    std::string name = rest, field = dot + 1;
                    if (!name.empty())
                    {
                        LspExtraServer &srv = extraServer(s, name);
                        if (field == "command")
                            srv.command = val;
                        else if (field == "langs")
                            srv.languages = splitList(val);
                    }
                }
            }
        }
        else if (strncmp(line, serverPrefix, sizeof serverPrefix - 1) == 0)
        {
            // lsp.server.<lang>=<command>
            char *rest = line + sizeof serverPrefix - 1;
            char *eq = strchr(rest, '=');
            if (eq)
            {
                *eq = '\0';
                char *cmd = eq + 1;
                chomp(cmd);
                if (rest[0] && cmd[0])
                    s.setLspCommand(rest, cmd);
            }
        }
    }
    fclose(f);
}

void saveSettings(const AppSettings &s) noexcept
{
    auto path = settingsPath();
    if (path.empty())
        return;
    FILE *f = fopen(path.c_str(), "w");
    if (!f)
        return;
    fprintf(f, "autosave=%d\n", s.autoSaveOnFocusLoss ? 1 : 0);
    fprintf(f, "lsp.enabled=%d\n", s.lspEnabled ? 1 : 0);
    fprintf(f, "showhidden=%d\n", s.showHidden ? 1 : 0);
    if (!s.treeIcons.empty())
        fprintf(f, "tree.icons=%s\n", s.treeIcons.c_str());
    if (!s.colorMode.empty())
        fprintf(f, "theme.colors=%s\n", s.colorMode.c_str());
    if (!s.defaultAgent.empty())
        fprintf(f, "agent.default=%s\n", s.defaultAgent.c_str());
    for (auto &srv : s.lspServers)
        if (!srv.language.empty() && !srv.command.empty())
            fprintf(f, "lsp.server.%s=%s\n", srv.language.c_str(), srv.command.c_str());
    for (auto &e : s.lspExtraServers)
        if (!e.name.empty() && !e.command.empty())
        {
            fprintf(f, "lsp.extra.%s.command=%s\n", e.name.c_str(), e.command.c_str());
            if (!e.languages.empty())
            {
                std::string csv;
                for (size_t i = 0; i < e.languages.size(); ++i)
                    csv += (i ? "," : "") + e.languages[i];
                fprintf(f, "lsp.extra.%s.langs=%s\n", e.name.c_str(), csv.c_str());
            }
        }
    for (auto &kv : s.theme)
        if (!kv.first.empty())
            fprintf(f, "theme.%s=%s\n", kv.first.c_str(), kv.second.c_str());
    fclose(f);
}

bool terminalAdvertisesTrueColor() noexcept
{
    const char *ct = getenv("COLORTERM");
    return ct && (std::strcmp(ct, "truecolor") == 0 || std::strcmp(ct, "24bit") == 0);
}

void applyColorDepthPreference() noexcept
{
    AppSettings s;
    loadSettings(s);
    // Pin Turbo Vision's colour depth to 16 for the classic fallback, so even
    // views that draw with raw RGB attributes (the file tree, debug panels)
    // render through the terminal's own 16-colour palette. "16" always; on
    // Windows "auto" too, unless the terminal advertises truecolor -- the
    // classic console reports truecolor but renders it poorly.
    bool cap = (s.colorMode == "16");
#ifdef _WIN32
    if (s.colorMode == "auto" && !terminalAdvertisesTrueColor())
        cap = true;
#endif
    if (cap)
    {
#ifdef _WIN32
        _putenv_s("TVISION_COLORS", "16");
#else
        setenv("TVISION_COLORS", "16", 1);
#endif
    }
}
