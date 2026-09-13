#include "pico/WasapiPicoRenderDevice.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propidl.h>
#include <propkeydef.h>
#include <mmreg.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>

namespace audient::pico
{
namespace
{

// --- Device property keys (defined locally to avoid extra link deps) ---------
// PKEY_Device_InstanceId {b3f8fa53-0004-438e-9003-51a46e139bfc}, 6
const PROPERTYKEY kPkeyInstanceId = {
    {0xb3f8fa53, 0x0004, 0x438e, {0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc}}, 6};
// PKEY_Device_FriendlyName {a45c254e-df1c-4efd-8020-67d146a850e0}, 14
const PROPERTYKEY kPkeyFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

// USB Audio Class 2 function subtypes.
const GUID kSubtypeIeeeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

// Accepted Audient Console Bridge USB identities. RP2040 = PID_B2DC (original
// accepted build); RP2350 = PID_B2DD (moved device, same UAC2 endpoint contract).
// Matching is by instance-id substring so both are found; the friendly-name
// fallback below stays as the guarded secondary match.
constexpr const wchar_t* kPicoHardwareIds[] = {L"VID_1209&PID_B2DC", L"VID_1209&PID_B2DD"};
constexpr wchar_t kPicoFriendlyName[] = L"audient console bridge";

bool containsInsensitive(const std::wstring& haystack, const wchar_t* needle)
{
    if (needle == nullptr || *needle == L'\0')
    {
        return false;
    }
    std::wstring lowered;
    lowered.reserve(haystack.size());
    for (wchar_t c : haystack)
    {
        lowered.push_back(static_cast<wchar_t>(towlower(c)));
    }
    std::wstring loweredNeedle;
    for (const wchar_t* p = needle; *p != L'\0'; ++p)
    {
        loweredNeedle.push_back(static_cast<wchar_t>(towlower(*p)));
    }
    return lowered.find(loweredNeedle) != std::wstring::npos;
}

std::string wideToUtf8(const wchar_t* w)
{
    if (w == nullptr)
    {
        return std::string();
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1)
    {
        return std::string();
    }
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

bool isDeviceLost(HRESULT hr)
{
    return hr == AUDCLNT_E_DEVICE_INVALIDATED ||
           hr == AUDCLNT_E_RESOURCES_INVALIDATED ||
           hr == AUDCLNT_E_ENDPOINT_CREATE_FAILED;
}

} // namespace

// --- IMMNotificationClient: only flags a teardown/re-discover -----------------
struct PicoEndpointNotificationClient final : public IMMNotificationClient
{
    explicit PicoEndpointNotificationClient(std::atomic<bool>* flag)
        : m_flag(flag)
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr)
        {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient))
        {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refCount; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --m_refCount;
        if (remaining == 0)
        {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override
    {
        m_flag->store(true, std::memory_order_release);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override
    {
        m_flag->store(true, std::memory_order_release);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override
    {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override
    {
        return S_OK;
    }

private:
    std::atomic<ULONG> m_refCount{1};
    std::atomic<bool>* m_flag;
};

WasapiPicoRenderDevice::~WasapiPicoRenderDevice()
{
    close();
}

PicoDeviceOpenResult WasapiPicoRenderDevice::open()
{
    PicoDeviceOpenResult result;

    if (!m_comInitialized)
    {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr))
        {
            result.error = "CoInitializeEx failed";
            return result;
        }
        m_comInitialized = true;
    }
    if (m_enumerator == nullptr)
    {
        const HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                            __uuidof(IMMDeviceEnumerator),
                                            reinterpret_cast<void**>(&m_enumerator));
        if (FAILED(hr) || m_enumerator == nullptr)
        {
            result.error = "IMMDeviceEnumerator creation failed";
            return result;
        }
    }

    IMMDeviceCollection* collection = nullptr;
    HRESULT hr = m_enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr) || collection == nullptr)
    {
        result.error = "EnumAudioEndpoints(eRender) failed";
        return result;
    }

    const auto match = [&](bool byHardwareId, IMMDevice** out, std::wstring* outName) -> int {
        UINT count = 0;
        collection->GetCount(&count);
        int matches = 0;
        for (UINT i = 0; i < count; ++i)
        {
            IMMDevice* dev = nullptr;
            if (FAILED(collection->Item(i, &dev)) || dev == nullptr)
            {
                continue;
            }
            IPropertyStore* props = nullptr;
            std::wstring instanceId;
            std::wstring friendly;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props != nullptr)
            {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(props->GetValue(kPkeyInstanceId, &pv)) && pv.vt == VT_LPWSTR &&
                    pv.pwszVal != nullptr)
                {
                    instanceId = pv.pwszVal;
                }
                PropVariantClear(&pv);
                PropVariantInit(&pv);
                if (SUCCEEDED(props->GetValue(kPkeyFriendlyName, &pv)) && pv.vt == VT_LPWSTR &&
                    pv.pwszVal != nullptr)
                {
                    friendly = pv.pwszVal;
                }
                PropVariantClear(&pv);
                props->Release();
            }

            bool hardwareMatch = false;
            for (const wchar_t* id : kPicoHardwareIds)
            {
                if (containsInsensitive(instanceId, id))
                {
                    hardwareMatch = true;
                    break;
                }
            }
            const bool isMatch = byHardwareId ? hardwareMatch
                                              : containsInsensitive(friendly, kPicoFriendlyName);
            if (isMatch)
            {
                if (matches == 0)
                {
                    *out = dev; // adopt
                    *outName = friendly;
                }
                else
                {
                    dev->Release();
                }
                ++matches;
            }
            else
            {
                dev->Release();
            }
        }
        return matches;
    };

    IMMDevice* device = nullptr;
    std::wstring name;
    int matches = match(true, &device, &name);
    if (matches == 0)
    {
        matches = match(false, &device, &name); // guarded friendly-name fallback
    }
    collection->Release();

    if (matches == 0)
    {
        result.error = "Pico UAC2 render endpoint not found (Audient Console Bridge; "
                       "VID_1209&PID_B2DC|B2DD)";
        return result;
    }
    if (matches > 1)
    {
        if (device != nullptr)
        {
            device->Release();
        }
        result.error = "Pico UAC2 render endpoint is ambiguous (multiple matches)";
        return result;
    }
    m_device = device;
    m_name = wideToUtf8(name.c_str());
    if (m_name.empty())
    {
        m_name = "Speakers (Audient Console Bridge)";
    }

    hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                            reinterpret_cast<void**>(&m_client));
    if (FAILED(hr) || m_client == nullptr)
    {
        result.error = "IAudioClient activation failed";
        close();
        return result;
    }

    WAVEFORMATEX* mix = nullptr;
    hr = m_client->GetMixFormat(&mix);
    if (FAILED(hr) || mix == nullptr)
    {
        result.error = "GetMixFormat failed";
        close();
        return result;
    }
    m_mix = mix;

    m_channels = mix->nChannels;
    m_sampleRate = mix->nSamplesPerSec;
    m_bitsPerSample = mix->wBitsPerSample;
    bool isFloat = false;
    if (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
    {
        isFloat = true;
    }
    else if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
        isFloat = IsEqualGUID(ext->SubFormat, kSubtypeIeeeFloat) != 0;
    }
    else if (mix->wFormatTag == WAVE_FORMAT_PCM)
    {
        isFloat = false;
    }
    m_isFloat = isFloat;

    // The Pico v0.1.1 UAC2 contract is fixed 48 kHz stereo. Do not silently
    // resample or truncate: fail safe (silence) and report instead.
    if (m_sampleRate != 48000u || m_channels != 2u)
    {
        result.error = "Pico render mix format is not 48 kHz stereo";
        close();
        return result;
    }

    constexpr REFERENCE_TIME kRequestedDuration = 0; // engine-chosen (shared)
    hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                              kRequestedDuration, 0, mix, nullptr);
    if (FAILED(hr))
    {
        // Some engines reject a zero duration; retry with a small bounded one.
        constexpr REFERENCE_TIME kFallbackDuration = 20 * 10000; // 20 ms
        hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                  kFallbackDuration, 0, mix, nullptr);
    }
    if (FAILED(hr))
    {
        result.error = "IAudioClient::Initialize failed";
        close();
        return result;
    }

    m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (m_event == nullptr)
    {
        result.error = "CreateEvent failed";
        close();
        return result;
    }
    hr = m_client->SetEventHandle(static_cast<HANDLE>(m_event));
    if (FAILED(hr))
    {
        result.error = "SetEventHandle failed";
        close();
        return result;
    }

    hr = m_client->GetBufferSize(&m_bufferFrames);
    if (FAILED(hr))
    {
        result.error = "GetBufferSize failed";
        close();
        return result;
    }

    hr = m_client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&m_render));
    if (FAILED(hr) || m_render == nullptr)
    {
        result.error = "IAudioRenderClient service unavailable";
        close();
        return result;
    }

    m_notify = new PicoEndpointNotificationClient(&m_needsReopen);
    m_enumerator->RegisterEndpointNotificationCallback(m_notify);

    hr = m_client->Start();
    if (FAILED(hr))
    {
        result.error = "IAudioClient::Start failed";
        close();
        return result;
    }
    m_started = true;
    m_needsReopen.store(false, std::memory_order_release);

    result.ok = true;
    result.deviceName = m_name;
    return result;
}

void WasapiPicoRenderDevice::close()
{
    if (m_enumerator != nullptr && m_notify != nullptr)
    {
        m_enumerator->UnregisterEndpointNotificationCallback(m_notify);
    }
    if (m_notify != nullptr)
    {
        m_notify->Release();
        m_notify = nullptr;
    }
    if (m_client != nullptr && m_started)
    {
        m_client->Stop();
        m_started = false;
    }
    if (m_render != nullptr)
    {
        m_render->Release();
        m_render = nullptr;
    }
    if (m_client != nullptr)
    {
        m_client->Release();
        m_client = nullptr;
    }
    if (m_device != nullptr)
    {
        m_device->Release();
        m_device = nullptr;
    }
    if (m_mix != nullptr)
    {
        CoTaskMemFree(m_mix);
        m_mix = nullptr;
    }
    if (m_event != nullptr)
    {
        CloseHandle(static_cast<HANDLE>(m_event));
        m_event = nullptr;
    }
    m_bufferFrames = 0;
    m_needsReopen.store(false, std::memory_order_release);
}

bool WasapiPicoRenderDevice::isOpen() const
{
    return m_client != nullptr && m_started && m_render != nullptr;
}

bool WasapiPicoRenderDevice::needsReopen() const
{
    return m_needsReopen.load(std::memory_order_acquire);
}

bool WasapiPicoRenderDevice::waitForRenderReady(unsigned timeoutMs)
{
    if (!isOpen())
    {
        return false;
    }
    const DWORD waited = WaitForSingleObject(static_cast<HANDLE>(m_event), timeoutMs);
    if (m_needsReopen.load(std::memory_order_acquire))
    {
        return false;
    }
    return waited == WAIT_OBJECT_0;
}

void WasapiPicoRenderDevice::wake()
{
    if (m_event != nullptr)
    {
        SetEvent(static_cast<HANDLE>(m_event));
    }
}

unsigned WasapiPicoRenderDevice::availableRenderFrames()
{
    if (!isOpen())
    {
        return 0;
    }
    UINT32 padding = 0;
    const HRESULT hr = m_client->GetCurrentPadding(&padding);
    if (FAILED(hr))
    {
        if (isDeviceLost(hr))
        {
            m_needsReopen.store(true, std::memory_order_release);
        }
        return 0;
    }
    if (padding >= m_bufferFrames)
    {
        return 0;
    }
    return static_cast<unsigned>(m_bufferFrames - padding);
}

bool WasapiPicoRenderDevice::submit(const float* stereoInterleaved, std::size_t frames,
                                    bool* deviceLost)
{
    if (deviceLost != nullptr)
    {
        *deviceLost = false;
    }
    if (!isOpen() || stereoInterleaved == nullptr || frames == 0)
    {
        return false;
    }
    unsigned toWrite = static_cast<unsigned>(frames);
    const unsigned available = availableRenderFrames();
    if (toWrite > available)
    {
        toWrite = available;
    }
    if (toWrite == 0)
    {
        return true;
    }

    BYTE* buffer = nullptr;
    HRESULT hr = m_render->GetBuffer(toWrite, &buffer);
    if (FAILED(hr) || buffer == nullptr)
    {
        if (isDeviceLost(hr) && deviceLost != nullptr)
        {
            *deviceLost = true;
        }
        return false;
    }
    writeFrames(buffer, stereoInterleaved, toWrite);
    hr = m_render->ReleaseBuffer(toWrite, 0);
    if (FAILED(hr))
    {
        if (isDeviceLost(hr) && deviceLost != nullptr)
        {
            *deviceLost = true;
        }
        return false;
    }
    return true;
}

void WasapiPicoRenderDevice::writeFrames(unsigned char* dst, const float* src,
                                         unsigned frames) const
{
    const std::size_t samples = static_cast<std::size_t>(frames) * 2u;
    if (m_isFloat)
    {
        std::memcpy(dst, src, samples * sizeof(float));
        return;
    }
    auto* out = reinterpret_cast<std::int16_t*>(dst);
    for (std::size_t i = 0; i < samples; ++i)
    {
        float v = src[i];
        if (v > 1.0f)
        {
            v = 1.0f;
        }
        else if (v < -1.0f)
        {
            v = -1.0f;
        }
        out[i] = static_cast<std::int16_t>(std::lround(v * 32767.0f));
    }
}

unsigned WasapiPicoRenderDevice::sampleRate() const
{
    return m_sampleRate;
}

unsigned WasapiPicoRenderDevice::channels() const
{
    return m_channels;
}

const std::string& WasapiPicoRenderDevice::deviceName() const
{
    return m_name;
}

void WasapiPicoRenderDevice::threadShutdown()
{
    close();
    if (m_comInitialized)
    {
        CoUninitialize();
        m_comInitialized = false;
    }
}

} // namespace audient::pico
