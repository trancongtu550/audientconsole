#include "preferences/AppPreferences.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace audient::preferences
{
namespace
{

// Minimal, bounded, tolerant JSON scanner for the flat preferences schema. It
// validates the whole document and extracts only the known top-level keys;
// unknown keys and value types are skipped (forward compatibility). Any
// malformed input makes the scan fail, and the caller falls back to defaults.
class Scanner
{
public:
    Scanner(const char* begin, const char* end)
        : m_p(begin)
        , m_end(end)
    {
    }

    bool parse(AppPreferences& out)
    {
        skipWhitespace();
        if (!consume('{'))
        {
            return false;
        }
        skipWhitespace();
        if (consume('}'))
        {
            skipWhitespace();
            return atEnd();
        }
        for (;;)
        {
            std::string key;
            if (!parseString(key))
            {
                return false;
            }
            skipWhitespace();
            if (!consume(':'))
            {
                return false;
            }
            skipWhitespace();
            if (!parseKnownValue(key, out))
            {
                return false;
            }
            skipWhitespace();
            if (consume(','))
            {
                skipWhitespace();
                continue;
            }
            if (consume('}'))
            {
                skipWhitespace();
                return atEnd();
            }
            return false;
        }
    }

private:
    bool atEnd() const { return m_p == m_end; }
    bool consume(char c)
    {
        if (m_p != m_end && *m_p == c)
        {
            ++m_p;
            return true;
        }
        return false;
    }
    void skipWhitespace()
    {
        while (m_p != m_end)
        {
            const char c = *m_p;
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            {
                ++m_p;
            }
            else
            {
                break;
            }
        }
    }
    bool parseString(std::string& out)
    {
        out.clear();
        if (!consume('"'))
        {
            return false;
        }
        while (m_p != m_end)
        {
            const char c = *m_p++;
            if (c == '"')
            {
                return true;
            }
            if (c == '\\')
            {
                if (m_p == m_end)
                {
                    return false;
                }
                const char esc = *m_p++;
                switch (esc)
                {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u':
                    // Skip the 4 hex digits; keys are ASCII in our schema.
                    for (int i = 0; i < 4; ++i)
                    {
                        if (m_p == m_end)
                        {
                            return false;
                        }
                        ++m_p;
                    }
                    break;
                default:
                    return false;
                }
                continue;
            }
            out.push_back(c);
        }
        return false;
    }
    bool parseBool(bool& out)
    {
        if (matchLiteral("true"))
        {
            out = true;
            return true;
        }
        if (matchLiteral("false"))
        {
            out = false;
            return true;
        }
        return false;
    }
    bool matchLiteral(const char* literal)
    {
        const char* q = m_p;
        for (const char* p = literal; *p != '\0'; ++p)
        {
            if (q == m_end || *q != *p)
            {
                return false;
            }
            ++q;
        }
        m_p = q;
        return true;
    }
    bool parseInteger(int& out)
    {
        const char* start = m_p;
        if (m_p != m_end && (*m_p == '-' || *m_p == '+'))
        {
            ++m_p;
        }
        const char* digits = m_p;
        while (m_p != m_end && *m_p >= '0' && *m_p <= '9')
        {
            ++m_p;
        }
        if (m_p == digits)
        {
            m_p = start;
            return false;
        }
        const std::string text(start, m_p);
        out = std::atoi(text.c_str());
        return true;
    }

    bool parseKnownValue(const std::string& key, AppPreferences& out)
    {
        if (key == "virtualMicSource")
        {
            int value = 0;
            if (parseInteger(value) &&
                (value == kVirtualMicSourceInput1 || value == kVirtualMicSourceInput2))
            {
                out.virtualMicSource = value;
            }
            else
            {
                skipValue(); // tolerate an out-of-range/odd value
            }
            return true;
        }
        if (key == "theme8bit")
        {
            // Retired preference (8-bit theme removed from scope). Accepted and
            // ignored so existing settings.json files keep loading without error
            // and are never blanked; the value is simply not retained.
            skipValue();
            return true;
        }
        if (key == "closeToTray")
        {
            bool value = false;
            if (parseBool(value))
            {
                out.closeToTray = value;
            }
            else
            {
                skipValue();
            }
            return true;
        }
        if (key == "startMinimized")
        {
            bool value = false;
            if (parseBool(value))
            {
                out.startMinimized = value;
            }
            else
            {
                skipValue();
            }
            return true;
        }
        if (key == "uiMode")
        {
            int value = 0;
            if (parseInteger(value) && (value == kUiModeMain || value == kUiModeMini))
            {
                out.uiMode = value;
            }
            else
            {
                skipValue(); // tolerate an out-of-range/odd value
            }
            return true;
        }
        // Unknown key: skip its value entirely.
        return skipValue();
    }

    bool skipValue(int depth = 0)
    {
        if (depth > 32)
        {
            return false; // bounded nesting
        }
        if (m_p == m_end)
        {
            return false;
        }
        const char c = *m_p;
        if (c == '"')
        {
            std::string ignored;
            return parseString(ignored);
        }
        if (c == '{' || c == '[')
        {
            const char open = c;
            const char close = (open == '{') ? '}' : ']';
            ++m_p;
            skipWhitespace();
            if (consume(close))
            {
                return true;
            }
            for (;;)
            {
                skipWhitespace();
                if (open == '{')
                {
                    std::string ignoredKey;
                    if (!parseString(ignoredKey))
                    {
                        return false;
                    }
                    skipWhitespace();
                    if (!consume(':'))
                    {
                        return false;
                    }
                    skipWhitespace();
                }
                if (!skipValue(depth + 1))
                {
                    return false;
                }
                skipWhitespace();
                if (consume(','))
                {
                    continue;
                }
                return consume(close);
            }
        }
        if (matchLiteral("true") || matchLiteral("false") || matchLiteral("null"))
        {
            return true;
        }
        int ignored = 0;
        return parseInteger(ignored);
    }

    const char* m_p;
    const char* m_end;
};

constexpr std::size_t kMaxPreferencesBytes = 64 * 1024;

} // namespace

const char* virtualMicSourceName(int source)
{
    return source == kVirtualMicSourceInput2 ? "Input 2" : "Input 1";
}

std::string serializePreferences(const AppPreferences& preferences)
{
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": " << kPreferencesSchema << ",\n";
    out << "  \"virtualMicSource\": " << preferences.virtualMicSource << ",\n";
    out << "  \"closeToTray\": " << (preferences.closeToTray ? "true" : "false") << ",\n";
    out << "  \"startMinimized\": " << (preferences.startMinimized ? "true" : "false") << ",\n";
    out << "  \"uiMode\": " << preferences.uiMode << "\n";
    out << "}\n";
    return out.str();
}

AppPreferences parsePreferences(const std::string& text, std::string* warning)
{
    AppPreferences result; // safe defaults
    if (text.empty())
    {
        if (warning != nullptr)
        {
            *warning = "empty preferences document; using defaults";
        }
        return result;
    }
    if (text.size() > kMaxPreferencesBytes)
    {
        if (warning != nullptr)
        {
            *warning = "preferences document too large; using defaults";
        }
        return result;
    }
    Scanner scanner(text.data(), text.data() + text.size());
    if (!scanner.parse(result))
    {
        if (warning != nullptr)
        {
            *warning = "corrupt preferences document; using defaults";
        }
        return AppPreferences{};
    }
    return result;
}

std::string defaultPreferencesPath()
{
    std::string local;
#if defined(_MSC_VER)
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, "LOCALAPPDATA") != 0 || buffer == nullptr)
    {
        return std::string();
    }
    local.assign(buffer);
    free(buffer);
#else
    const char* env = std::getenv("LOCALAPPDATA");
    if (env == nullptr || *env == '\0')
    {
        return std::string();
    }
    local.assign(env);
#endif
    if (local.empty())
    {
        return std::string();
    }
    std::filesystem::path base(local);
    base /= "Audient Console";
    base /= "settings.json";
    return base.string();
}

bool loadPreferencesFromFile(const std::string& path, AppPreferences& out, std::string* error)
{
    out = AppPreferences{};
    if (path.empty())
    {
        if (error != nullptr)
        {
            *error = "no preferences path";
        }
        return true;
    }
    try
    {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec)
        {
            return true; // missing file is not an error: keep defaults
        }
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            if (error != nullptr)
            {
                *error = "could not open preferences file";
            }
            return false;
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        std::string warning;
        out = parsePreferences(buffer.str(), &warning);
        if (!warning.empty() && error != nullptr)
        {
            *error = warning;
        }
        return true;
    }
    catch (const std::exception& e)
    {
        if (error != nullptr)
        {
            *error = e.what();
        }
        out = AppPreferences{};
        return false;
    }
}

bool savePreferencesToFile(const std::string& path, const AppPreferences& preferences,
                           std::string* error)
{
    if (path.empty())
    {
        if (error != nullptr)
        {
            *error = "no preferences path";
        }
        return false;
    }
    try
    {
        const std::filesystem::path target(path);
        std::error_code ec;
        if (target.has_parent_path())
        {
            std::filesystem::create_directories(target.parent_path(), ec);
        }
        const std::string data = serializePreferences(preferences);
        std::filesystem::path temp = target;
        temp += ".tmp";
        {
            std::ofstream file(temp, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                if (error != nullptr)
                {
                    *error = "could not open temp preferences file";
                }
                return false;
            }
            file.write(data.data(), static_cast<std::streamsize>(data.size()));
            file.flush();
            if (!file)
            {
                if (error != nullptr)
                {
                    *error = "could not write temp preferences file";
                }
                return false;
            }
        }
        // std::filesystem::rename replaces an existing target atomically on
        // Windows (MoveFileEx with MOVEFILE_REPLACE_EXISTING).
        std::filesystem::rename(temp, target, ec);
        if (ec)
        {
            std::filesystem::remove(temp, ec);
            if (error != nullptr)
            {
                *error = "could not replace preferences file";
            }
            return false;
        }
        return true;
    }
    catch (const std::exception& e)
    {
        if (error != nullptr)
        {
            *error = e.what();
        }
        return false;
    }
}

bool loadPreferences(AppPreferences& out, std::string* error)
{
    return loadPreferencesFromFile(defaultPreferencesPath(), out, error);
}

bool savePreferences(const AppPreferences& preferences, std::string* error)
{
    return savePreferencesToFile(defaultPreferencesPath(), preferences, error);
}

} // namespace audient::preferences
