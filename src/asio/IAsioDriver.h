#pragma once

#include "asio/AsioTypes.h"

#include <memory>
#include <string>
#include <vector>

namespace audient::asio
{

class IAsioDriver
{
public:
    virtual ~IAsioDriver() = default;

    virtual DriverCapabilities capabilities() const = 0;
    virtual bool initDriver(long sampleRate, std::string& error) = 0;
    virtual bool prepareBuffers(long bufferSamples, const std::vector<ChannelInfo>& inputs,
                                const std::vector<ChannelInfo>& outputs, std::string& error) = 0;
    virtual bool setCallback(AsioCallbackFn callback, void* context, std::string& error) = 0;
    virtual bool startStream(std::string& error) = 0;
    virtual bool stopStream(std::string& error) = 0;
    virtual void disposeDriver() = 0;

    // Read the driver's ACTUAL current sample rate (Hz). Non-pure: drivers that
    // cannot report it keep the default and return false. Used to reflect the
    // real device state and to detect an external (e.g. iD Mixer) rate change.
    virtual bool currentSampleRate(long& sampleRate) const
    {
        (void)sampleRate;
        return false;
    }

    // Read the driver's ACTUAL current buffer size (frames). Includes any value
    // the driver reported via an asynchronous buffer-size-change notification.
    // Non-pure; default reports unavailable.
    virtual bool currentBufferSize(long& bufferSamples) const
    {
        (void)bufferSamples;
        return false;
    }

    // Number of asynchronous buffer-size-change notifications received from the
    // driver's asioMessage() callback (diagnostic: did the driver tell us, or did
    // we only see the change via polling?). Non-pure; default 0.
    virtual unsigned bufferSizeChangeNotifyCount() const
    {
        return 0;
    }
};

class IAsioDriverProvider
{
public:
    virtual ~IAsioDriverProvider() = default;

    virtual std::vector<DriverIdentity> available() const = 0;
    virtual std::unique_ptr<IAsioDriver> open(const DriverIdentity& identity, std::string& error) = 0;
};

} // namespace audient::asio