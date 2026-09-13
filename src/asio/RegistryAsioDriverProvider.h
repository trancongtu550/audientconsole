#pragma once

#include "asio/IAsioDriver.h"

namespace audient::asio
{

class RegistryAsioDriverProvider : public IAsioDriverProvider
{
public:
    std::vector<DriverIdentity> available() const override;
    std::unique_ptr<IAsioDriver> open(const DriverIdentity& identity, std::string& error) override;
};

} // namespace audient::asio