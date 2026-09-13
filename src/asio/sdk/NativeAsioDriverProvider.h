#pragma once

#include "asio/IAsioDriver.h"
#include "asio/RegistryAsioDriverProvider.h"

#include <memory>
#include <string>
#include <vector>

namespace audient::asio
{

class NativeAsioDriverProvider : public IAsioDriverProvider
{
public:
    std::vector<DriverIdentity> available() const override;
    std::unique_ptr<IAsioDriver> open(const DriverIdentity& identity, std::string& error) override;

private:
    RegistryAsioDriverProvider m_registry;
};

} // namespace audient::asio