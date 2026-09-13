#include "DailyMixerState.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace audient::daily
{
namespace
{

std::wstring toWide(const std::string& s)
{
    if (s.empty())
    {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string toUtf8(const std::wstring& w)
{
    if (w.empty())
    {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring pathJoin(const std::wstring& a, const std::wstring& b)
{
    if (a.empty())
    {
        return b;
    }
    std::wstring r = a;
    if (r.back() != L'\\' && r.back() != L'/')
    {
        r += L'\\';
    }
    r += b;
    return r;
}

// ---- minimal JSON model + parser (subset used by mixer-state.json) ---------
struct Json
{
    enum class Type
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string str;
    std::vector<Json> array;
    std::vector<std::pair<std::string, Json>> object;

    const Json* find(const char* key) const
    {
        for (const auto& kv : object)
        {
            if (kv.first == key)
            {
                return &kv.second;
            }
        }
        return nullptr;
    }
};

struct Parser
{
    const std::string& s;
    std::size_t i = 0;
    bool ok = true;

    explicit Parser(const std::string& in) : s(in) {}

    void ws()
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n'))
        {
            ++i;
        }
    }
    bool eat(char c)
    {
        ws();
        if (i < s.size() && s[i] == c)
        {
            ++i;
            return true;
        }
        return false;
    }
    bool lit(const char* w)
    {
        const std::size_t n = std::strlen(w);
        if (s.compare(i, n, w) == 0)
        {
            i += n;
            return true;
        }
        return false;
    }
    std::string parseString()
    {
        std::string out;
        if (!eat('"'))
        {
            ok = false;
            return out;
        }
        while (i < s.size() && ok)
        {
            const char c = s[i++];
            if (c == '"')
            {
                return out;
            }
            if (c == '\\')
            {
                if (i >= s.size())
                {
                    break;
                }
                const char e = s[i++];
                switch (e)
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
                {
                    if (i + 4 > s.size())
                    {
                        ok = false;
                        break;
                    }
                    unsigned cp = 0;
                    for (int k = 0; k < 4; ++k)
                    {
                        const char h = s[i++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                        else { ok = false; break; }
                    }
                    if (cp < 0x80) out.push_back(static_cast<char>(cp));
                    else if (cp < 0x800)
                    {
                        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    else
                    {
                        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: ok = false; break;
                }
            }
            else if (static_cast<unsigned char>(c) < 0x20)
            {
                ok = false;
            }
            else
            {
                out.push_back(c);
            }
        }
        ok = false;
        return out;
    }
    Json parseValue()
    {
        ws();
        Json v;
        if (i >= s.size())
        {
            ok = false;
            return v;
        }
        const char c = s[i];
        if (c == '{')
        {
            ++i;
            v.type = Json::Type::Object;
            ws();
            if (eat('}'))
            {
                return v;
            }
            while (ok)
            {
                ws();
                std::string key = parseString();
                if (!ok || !eat(':'))
                {
                    ok = false;
                    break;
                }
                v.object.emplace_back(std::move(key), parseValue());
                if (eat(','))
                {
                    continue;
                }
                if (eat('}'))
                {
                    break;
                }
                ok = false;
            }
            return v;
        }
        if (c == '[')
        {
            ++i;
            v.type = Json::Type::Array;
            ws();
            if (eat(']'))
            {
                return v;
            }
            while (ok)
            {
                v.array.push_back(parseValue());
                if (eat(','))
                {
                    continue;
                }
                if (eat(']'))
                {
                    break;
                }
                ok = false;
            }
            return v;
        }
        if (c == '"')
        {
            v.type = Json::Type::String;
            v.str = parseString();
            return v;
        }
        if (lit("true"))
        {
            v.type = Json::Type::Bool;
            v.boolean = true;
            return v;
        }
        if (lit("false"))
        {
            v.type = Json::Type::Bool;
            v.boolean = false;
            return v;
        }
        if (lit("null"))
        {
            v.type = Json::Type::Null;
            return v;
        }
        if (c == '-' || (c >= '0' && c <= '9'))
        {
            const char* start = s.c_str() + i;
            char* end = nullptr;
            v.type = Json::Type::Number;
            v.number = std::strtod(start, &end);
            i += static_cast<std::size_t>(end - start);
            return v;
        }
        ok = false;
        return v;
    }
};

std::string jsonString(const std::string& s)
{
    std::string out;
    out.push_back('"');
    for (char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char b[8];
                std::snprintf(b, sizeof(b), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += b;
            }
            else
            {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
    return out;
}

const char* parseChannel(const Json* node, MixerChannel& out)
{
    if (node == nullptr || node->type != Json::Type::Object)
    {
        return nullptr;
    }
    if (const Json* wb = node->find("wholeBypass"))
    {
        if (wb->type == Json::Type::Bool)
        {
            out.wholeBypass = wb->boolean;
        }
    }
    const Json* inserts = node->find("inserts");
    if (inserts != nullptr && inserts->type == Json::Type::Array)
    {
        for (const Json& item : inserts->array)
        {
            if (item.type != Json::Type::Object)
            {
                continue;
            }
            MixerInsert ins;
            if (const Json* v = item.find("id"); v && v->type == Json::Type::String) ins.id = v->str;
            if (const Json* v = item.find("path"); v && v->type == Json::Type::String) ins.path = v->str;
            if (const Json* v = item.find("name"); v && v->type == Json::Type::String) ins.name = v->str;
            if (const Json* v = item.find("bypass"); v && v->type == Json::Type::Bool) ins.bypass = v->boolean;
            if (const Json* v = item.find("stateFile"); v && v->type == Json::Type::String) ins.stateFile = v->str;
            if (!ins.id.empty())
            {
                out.inserts.push_back(std::move(ins));
            }
        }
    }
    return "";
}

} // namespace

std::string appDataDir()
{
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2)
    {
        return {};
    }
    const std::wstring dir = pathJoin(buf, L"Audient Console");
    CreateDirectoryW(dir.c_str(), nullptr);
    return toUtf8(dir);
}

std::string mixerStatePath()
{
    const std::string dir = appDataDir();
    return dir.empty() ? std::string{} : dir + "\\mixer-state.json";
}

std::string stateBlobDir()
{
    const std::string dir = appDataDir();
    if (dir.empty())
    {
        return {};
    }
    const std::wstring w = pathJoin(toWide(dir), L"State");
    CreateDirectoryW(w.c_str(), nullptr);
    return toUtf8(w);
}

std::string stateBlobPath(const std::string& fileName)
{
    const std::string dir = stateBlobDir();
    return dir.empty() ? std::string{} : dir + "\\" + fileName;
}

std::string newInsertId()
{
    static std::mt19937_64 rng([] {
        std::random_device rd;
        std::uint64_t seed = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
        seed ^= static_cast<std::uint64_t>(GetTickCount64());
        return seed;
    }());
    const std::uint64_t v = rng();
    char b[32];
    std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(v));
    return b;
}

std::string serializeMixerState(const MixerState& state)
{
    std::ostringstream o;
    o << "{\n  \"schema\": " << state.schema << ",\n";
    auto writeChannel = [&](const char* key, const MixerChannel& ch, bool last) {
        o << "  \"" << key << "\": {\n    \"wholeBypass\": " << (ch.wholeBypass ? "true" : "false")
          << ",\n    \"inserts\": [";
        for (std::size_t i = 0; i < ch.inserts.size(); ++i)
        {
            const MixerInsert& in = ch.inserts[i];
            o << (i == 0 ? "\n" : ",\n") << "      {\"id\": " << jsonString(in.id)
              << ", \"path\": " << jsonString(in.path)
              << ", \"name\": " << jsonString(in.name)
              << ", \"bypass\": " << (in.bypass ? "true" : "false")
              << ", \"stateFile\": " << jsonString(in.stateFile) << "}";
        }
        if (!ch.inserts.empty())
        {
            o << "\n    ";
        }
        o << "]\n  }" << (last ? "\n" : ",\n");
    };
    writeChannel("ch0", state.ch0, false);
    writeChannel("ch1", state.ch1, true);
    o << "}\n";
    return o.str();
}

bool parseMixerState(const std::string& json, MixerState& out, std::string& error)
{
    Parser p(json);
    Json root = p.parseValue();
    if (!p.ok || root.type != Json::Type::Object)
    {
        error = "malformed mixer-state JSON";
        return false;
    }
    const Json* schema = root.find("schema");
    if (schema != nullptr && schema->type == Json::Type::Number)
    {
        const int v = static_cast<int>(schema->number);
        if (v != 1)
        {
            error = "unsupported mixer-state schema " + std::to_string(v);
            return false;
        }
        out.schema = v;
    }
    parseChannel(root.find("ch0"), out.ch0);
    parseChannel(root.find("ch1"), out.ch1);
    error.clear();
    return true;
}

bool loadMixerState(const std::string& path, MixerState& out, std::string& error)
{
    std::vector<unsigned char> data;
    if (!readBinaryFile(path, data))
    {
        error = "not found";
        return false;
    }
    const std::string json(data.begin(), data.end());
    return parseMixerState(json, out, error);
}

bool saveMixerStateAtomic(const std::string& path, const MixerState& state, std::string& error)
{
    const std::string json = serializeMixerState(state);
    std::vector<unsigned char> bytes(json.begin(), json.end());
    return writeBinaryFileAtomic(path, bytes, error);
}

bool readBinaryFile(const std::string& path, std::vector<unsigned char>& out)
{
    const std::wstring w = toWide(path);
    FILE* f = nullptr;
    if (_wfopen_s(&f, w.c_str(), L"rb") != 0 || f == nullptr)
    {
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len < 0)
    {
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<std::size_t>(len));
    const std::size_t got = len > 0 ? std::fread(out.data(), 1, out.size(), f) : 0;
    std::fclose(f);
    if (got != out.size())
    {
        out.clear();
        return false;
    }
    return true;
}

bool writeBinaryFileAtomic(const std::string& path, const std::vector<unsigned char>& data,
                           std::string& error)
{
    if (path.empty())
    {
        error = "empty path";
        return false;
    }
    const std::wstring wpath = toWide(path);
    const std::wstring wtmp = wpath + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, wtmp.c_str(), L"wb") != 0 || f == nullptr)
    {
        error = "cannot open temp file";
        return false;
    }
    bool ok = true;
    if (!data.empty())
    {
        ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    }
    if (ok)
    {
        ok = std::fflush(f) == 0;
    }
    if (ok)
    {
        ok = _commit(_fileno(f)) == 0;
    }
    std::fclose(f);
    if (!ok)
    {
        DeleteFileW(wtmp.c_str());
        error = "write/flush failed";
        return false;
    }
    if (!MoveFileExW(wtmp.c_str(), wpath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(wtmp.c_str());
        error = "atomic replace failed";
        return false;
    }
    return true;
}

} // namespace audient::daily
