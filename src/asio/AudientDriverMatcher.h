#pragma once

#include "asio/AsioTypes.h"

#include <string>
#include <vector>

namespace audient::asio
{

class AudientDriverMatcher
{
public:
    void rememberPreferredIdentity(std::string clsid);
    bool match(const std::vector<DriverIdentity>& available, DriverIdentity& out) const;

private:
    std::string m_preferredClsid;
};

} // namespace audient::asio