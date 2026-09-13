#include "channel/ChannelIdentity.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

namespace audient::channel
{

namespace
{

constexpr unsigned kMaxPhysicalOrdinal = 64;

std::string toLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool isAnalogueFamily(const std::string& token)
{
    return token == "analogue" || token == "analog" || token == "input" || token == "mic" ||
           token == "microphone" || token == "line" || token == "mono";
}

bool isNonInputFamily(const std::string& token)
{
    return token == "output" || token == "out" || token == "speaker" || token == "headphone" ||
           token == "monitor" || token == "adapter" || token == "ad" || token == "spdif" ||
           token == "adat" || token == "daw" || token == "cue" || token == "s/pdif";
}

bool isDigitString(const std::string& token)
{
    if (token.empty())
    {
        return false;
    }
    return std::all_of(token.begin(), token.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

unsigned parseOrdinalToken(const std::string& family, const std::string& rest)
{
    if (rest.empty())
    {
        return 0;
    }
    if (isDigitString(rest))
    {
        return static_cast<unsigned>(std::stoul(rest));
    }
    if (rest.size() > family.size() && rest.compare(0, family.size(), family) == 0 &&
        isDigitString(rest.substr(family.size())))
    {
        return static_cast<unsigned>(std::stoul(rest.substr(family.size())));
    }
    return 0;
}

} // namespace

ChannelIdentity makeAnalogInput(unsigned ordinal)
{
    ChannelIdentity identity;
    if (ordinal == 0 || ordinal > kMaxPhysicalOrdinal)
    {
        return identity;
    }
    identity.ordinal = ordinal;
    identity.stableName = "Analog Input " + std::to_string(ordinal);
    return identity;
}

ChannelIdentity parsePhysicalInputName(const std::string& channelName)
{
    ChannelIdentity identity;
    const std::string lowered = toLower(channelName);

    std::string token;
    std::size_t pos = 0;
    while (pos <= lowered.size())
    {
        const std::size_t space = lowered.find(' ', pos);
        token = lowered.substr(pos, space == std::string::npos ? std::string::npos : space - pos);
        if (token.empty())
        {
            if (space == std::string::npos)
            {
                break;
            }
            pos = space + 1;
            continue;
        }
        if (isNonInputFamily(token))
        {
            return identity;
        }
        if (isAnalogueFamily(token))
        {
            std::size_t after = space == std::string::npos ? lowered.size() : space;
            std::string number;
            std::size_t scan = after;
            while (scan < lowered.size() && lowered[scan] == ' ')
            {
                ++scan;
            }
            const std::size_t nextSpace = lowered.find(' ', scan);
            if (nextSpace == std::string::npos)
            {
                number = lowered.substr(scan);
            }
            else
            {
                number = lowered.substr(scan, nextSpace - scan);
            }
            const unsigned ordinal = parseOrdinalToken(token, number);
            if (ordinal == 0 || ordinal > kMaxPhysicalOrdinal)
            {
                return identity;
            }
            return makeAnalogInput(ordinal);
        }
        if (space == std::string::npos)
        {
            break;
        }
        pos = space + 1;
    }
    return identity;
}

} // namespace audient::channel
