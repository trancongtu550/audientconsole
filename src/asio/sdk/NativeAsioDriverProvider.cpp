#include "asio/sdk/NativeAsioDriverProvider.h"

#include "asio/sdk/NativeAsioDriver.h"

namespace audient::asio
{

std::vector<DriverIdentity> NativeAsioDriverProvider::available() const
{
    return m_registry.available();
}

std::unique_ptr<IAsioDriver> NativeAsioDriverProvider::open(const DriverIdentity& identity, std::string& error)
{
    error.clear();
    auto driver = std::make_unique<NativeAsioDriver>(identity);
    if (!driver->open(error))
    {
        if (error.empty())
        {
            error = "failed to open ASIO driver " + identity.name;
        }
        return nullptr;
    }
    return driver;
}

} // namespace audient::asio