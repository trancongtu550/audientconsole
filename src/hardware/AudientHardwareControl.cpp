// AudientHardwareControl.cpp - proven iD14 MK1 Monitor/Headphone volume control
// via the official Audient user-mode API (ADR-008). Shared production module;
// never called from the ASIO callback. Mute/dim/etc. deliberately unsupported.

#include "hardware/AudientHardwareControl.h"
#include "hardware/VolumeCodec.h"

#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <devguid.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace audient::hardware
{

namespace
{

const char* kDllPrimary =
    "C:\\Program Files\\Audient\\USBAudioDriver\\x64\\audientusbaudioapi_x64.dll";
const char* kDllFallback = "audientusbaudioapi_x64.dll";

constexpr std::uint32_t kSide = 0x01;

// Monitor volume (entity 0x36): GET/SET arg5 = 0x00 (ADR-008).
constexpr std::uint32_t kMonEntity = 0x36;
constexpr std::uint32_t kMonVolumeControl = 0x12;
constexpr std::uint32_t kMonArg5 = 0x00;

// Headphone volume (entity 0x0A): GET arg5 = 0x03; SET is the 3+4 pair.
constexpr std::uint32_t kHpEntity = 0x0A;
constexpr std::uint32_t kHpVolumeControl = 0x02;
constexpr std::uint32_t kHpGetArg5 = 0x03;
constexpr std::uint32_t kHpSetArg5A = 0x03;
constexpr std::uint32_t kHpSetArg5B = 0x04;

constexpr std::uint32_t kArg7Bytes = 2;   // int16 Q8.8 payload length
constexpr std::uint32_t kTimeoutMs = 10000; // official arg9
constexpr std::int32_t kReadBackTol256 = 16; // ~0.0625 dB raw-grid tolerance

using FnEnumerateDevices = int (*)();
using FnGetDeviceCount = int (*)();
using FnOpenDeviceByIndex = int (*)(int index, std::uint32_t* outHandle);
using FnCloseDevice = int (*)(std::uint32_t handle);
using FnControlRequest = int (*)(std::uint32_t handle, std::uint32_t entity,
                                 std::uint32_t side, std::uint32_t control,
                                 std::uint8_t arg5, void* arg6, std::uint32_t arg7,
                                 void* arg8, std::uint32_t arg9);

// Count present MEDIA-class devices matching the iD14 MK1 (VID_2708/PID_0002).
// Returns count >= 0, or -1 when the PnP query itself failed.
int countPresentMk1Devices()
{
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_MEDIA, nullptr, nullptr,
                                        DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
    {
        return -1;
    }
    int count = 0;
    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof(info);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); ++i)
    {
        bool match = false;
        for (DWORD prop = SPDRP_HARDWAREID; prop <= SPDRP_COMPATIBLEIDS && !match; ++prop)
        {
            WCHAR raw[512]{};
            DWORD need = 0;
            if (SetupDiGetDeviceRegistryPropertyW(
                    set, &info, prop, nullptr, reinterpret_cast<PBYTE>(raw),
                    sizeof(raw) - sizeof(WCHAR), &need) &&
                need > 0)
            {
                for (WCHAR* p = raw; *p; p += wcslen(p) + 1)
                {
                    if (wcsstr(p, L"VID_2708&PID_0002") != nullptr)
                    {
                        match = true;
                        break;
                    }
                }
            }
        }
        if (match)
        {
            ++count;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return count;
}

void packQ88(std::uint8_t out[2], std::int16_t raw)
{
    out[0] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(raw) & 0xFFu);
    out[1] = static_cast<std::uint8_t>((static_cast<std::uint16_t>(raw) >> 8) & 0xFFu);
}

} // namespace

const char* resultName(Result result)
{
    switch (result)
    {
    case Result::Ok: return "Ok";
    case Result::NotOpen: return "NotOpen";
    case Result::DllLoadFailed: return "DllLoadFailed";
    case Result::ExportMissing: return "ExportMissing";
    case Result::DeviceNotFound: return "DeviceNotFound";
    case Result::DeviceAmbiguous: return "DeviceAmbiguous";
    case Result::OpenFailed: return "OpenFailed";
    case Result::Disconnected: return "Disconnected";
    case Result::InvalidDb: return "InvalidDb";
    case Result::WriteFailed: return "WriteFailed";
    case Result::VerifyFailed: return "VerifyFailed";
    case Result::InternalError: return "InternalError";
    }
    return "Unknown";
}

struct AudientHardwareControl::Impl
{
    mutable std::mutex mu;

    HMODULE dll = nullptr;
    FnEnumerateDevices enumerate = nullptr;
    FnGetDeviceCount getDeviceCount = nullptr;
    FnOpenDeviceByIndex openByIndex = nullptr;
    FnCloseDevice closeDevice = nullptr;
    FnControlRequest requestGet = nullptr;
    FnControlRequest requestSet = nullptr;

    std::uint32_t handle = 0;
    std::optional<double> monDb;
    std::optional<double> hpDb;
    std::string error;
    std::chrono::steady_clock::time_point nextReopen{};

    unsigned long long ticks = 0;
    double lastPollMs = 0.0;

    std::atomic<bool> started{false};
    std::atomic<bool> connected{false};
    std::atomic<unsigned> periodMs{200}; // 5 Hz production default (A/B/C/D evidence)
    std::thread worker;
};

AudientHardwareControl::AudientHardwareControl()
    : impl_(new Impl())
{
}

AudientHardwareControl::~AudientHardwareControl()
{
    close();
    delete impl_;
    impl_ = nullptr;
}

bool AudientHardwareControl::isOpen() const
{
    return impl_->started.load(std::memory_order_acquire);
}

bool AudientHardwareControl::connected() const
{
    return impl_->connected.load(std::memory_order_acquire);
}

std::optional<double> AudientHardwareControl::monitorDb() const
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (!impl_->connected.load(std::memory_order_relaxed))
    {
        return std::nullopt;
    }
    return impl_->monDb;
}

std::optional<double> AudientHardwareControl::headphoneDb() const
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (!impl_->connected.load(std::memory_order_relaxed))
    {
        return std::nullopt;
    }
    return impl_->hpDb;
}

void AudientHardwareControl::setPollPeriodMs(unsigned periodMs)
{
    if (periodMs < 10)
    {
        periodMs = 10;
    }
    if (periodMs > 5000)
    {
        periodMs = 5000;
    }
    impl_->periodMs.store(periodMs, std::memory_order_relaxed);
}

unsigned AudientHardwareControl::pollPeriodMs() const
{
    return impl_->periodMs.load(std::memory_order_relaxed);
}

double AudientHardwareControl::lastPollMs() const
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->lastPollMs;
}

unsigned long long AudientHardwareControl::pollTicks() const
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->ticks;
}

std::string AudientHardwareControl::lastError() const
{
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->error;
}

// ---- static helpers (run on worker or under the Impl lock) ----------------

int AudientHardwareControl::readLevelLocked(Impl& impl, std::uint32_t entity,
                                            std::uint32_t control, std::uint32_t arg5,
                                            double* outDb)
{
    std::uint8_t payload[16]{};
    std::uint8_t scratch[16]{};
    const int st = impl.requestGet(impl.handle, entity, kSide, control,
                                   static_cast<std::uint8_t>(arg5), payload, kArg7Bytes,
                                   scratch, kTimeoutMs);
    if (st != 0)
    {
        return -1;
    }
    const std::uint16_t raw = static_cast<std::uint16_t>(
        payload[0] | (static_cast<std::uint16_t>(payload[1]) << 8));
    const std::optional<double> db = detail::validateReadRaw(raw);
    if (!db.has_value())
    {
        return -2;
    }
    *outDb = *db;
    return 0;
}

void AudientHardwareControl::markDeviceLostLocked(Impl& impl)
{
    if (impl.handle != 0 && impl.closeDevice != nullptr)
    {
        (void)impl.closeDevice(impl.handle);
    }
    impl.handle = 0;
    impl.connected.store(false, std::memory_order_relaxed);
    impl.monDb.reset();
    impl.hpDb.reset();
    impl.nextReopen = std::chrono::steady_clock::now() + std::chrono::seconds(1);
}

void AudientHardwareControl::tryReopenLocked(Impl& impl)
{
    const auto now = std::chrono::steady_clock::now();
    if (impl.handle != 0 || impl.dll == nullptr || impl.enumerate == nullptr ||
        impl.getDeviceCount == nullptr || impl.openByIndex == nullptr)
    {
        return;
    }
    if (now < impl.nextReopen)
    {
        return;
    }
    impl.nextReopen = now + std::chrono::seconds(1);

    const int present = countPresentMk1Devices();
    if (present != 1)
    {
        impl.connected.store(false, std::memory_order_relaxed);
        return;
    }
    (void)impl.enumerate();
    const int n = impl.getDeviceCount();
    std::uint32_t h = 0;
    if (n == 1 && impl.openByIndex(0, &h) == 0 && h != 0)
    {
        impl.handle = h;
        impl.connected.store(true, std::memory_order_relaxed);
    }
}

void AudientHardwareControl::pollOnce(AudientHardwareControl& owner)
{
    Impl& impl = *owner.impl_;
    std::unique_lock<std::mutex> lock(impl.mu);
    if (!impl.started.load(std::memory_order_relaxed))
    {
        return;
    }
    if (impl.handle == 0)
    {
        tryReopenLocked(impl);
        return;
    }

    const auto t0 = std::chrono::steady_clock::now();
    double mon = 0.0;
    double hp = 0.0;
    const int monSt = readLevelLocked(impl, kMonEntity, kMonVolumeControl, kMonArg5, &mon);
    const int hpSt = readLevelLocked(impl, kHpEntity, kHpVolumeControl, kHpGetArg5, &hp);
    if (monSt != 0 || hpSt != 0)
    {
        // Unreachable - close and let tryReopenLocked recover; hardware stays the
        // source of truth (we never push cached values on reconnect).
        markDeviceLostLocked(impl);
        return;
    }
    const auto t1 = std::chrono::steady_clock::now();
    impl.monDb = mon;
    impl.hpDb = hp;
    impl.connected.store(true, std::memory_order_relaxed);
    impl.lastPollMs =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    ++impl.ticks;
    impl.error.clear();
}

void AudientHardwareControl::pollWorker(AudientHardwareControl& owner)
{
    auto next = std::chrono::steady_clock::now();
    while (owner.impl_->started.load(std::memory_order_acquire))
    {
        const auto now = std::chrono::steady_clock::now();
        if (now < next)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            continue;
        }
        pollOnce(owner);
        next = std::chrono::steady_clock::now() +
               std::chrono::milliseconds(
                   owner.impl_->periodMs.load(std::memory_order_relaxed));
    }
}

// ---- public lifecycle ------------------------------------------------------

Result AudientHardwareControl::open()
{
    Impl& impl = *impl_;
    if (impl.started.load(std::memory_order_acquire))
    {
        return Result::Ok; // idempotent
    }

    HMODULE dll = LoadLibraryA(kDllPrimary);
    if (dll == nullptr)
    {
        dll = LoadLibraryA(kDllFallback);
    }
    if (dll == nullptr)
    {
        std::lock_guard<std::mutex> lock(impl.mu);
        impl.error = "official Audient API dll not found (driver not installed?)";
        return Result::DllLoadFailed;
    }

    FnEnumerateDevices enumerate = reinterpret_cast<FnEnumerateDevices>(
        GetProcAddress(dll, "TUSBAUDIO_EnumerateDevices"));
    FnGetDeviceCount getCount = reinterpret_cast<FnGetDeviceCount>(
        GetProcAddress(dll, "TUSBAUDIO_GetDeviceCount"));
    FnOpenDeviceByIndex openIdx = reinterpret_cast<FnOpenDeviceByIndex>(
        GetProcAddress(dll, "TUSBAUDIO_OpenDeviceByIndex"));
    FnCloseDevice closeDev = reinterpret_cast<FnCloseDevice>(
        GetProcAddress(dll, "TUSBAUDIO_CloseDevice"));
    FnControlRequest requestGet = reinterpret_cast<FnControlRequest>(
        GetProcAddress(dll, "TUSBAUDIO_AudioControlRequestGet"));
    FnControlRequest requestSet = reinterpret_cast<FnControlRequest>(
        GetProcAddress(dll, "TUSBAUDIO_AudioControlRequestSet"));

    if (enumerate == nullptr || getCount == nullptr || openIdx == nullptr ||
        closeDev == nullptr || requestGet == nullptr || requestSet == nullptr)
    {
        FreeLibrary(dll);
        std::lock_guard<std::mutex> lock(impl.mu);
        impl.error = "required TUSBAUDIO export missing from the API dll";
        return Result::ExportMissing;
    }

    const int present = countPresentMk1Devices();
    if (present == 0)
    {
        FreeLibrary(dll);
        std::lock_guard<std::mutex> lock(impl.mu);
        impl.error = "no Audient iD14 MK1 (VID_2708/PID_0002) detected";
        return Result::DeviceNotFound;
    }
    if (present != 1)
    {
        FreeLibrary(dll);
        std::lock_guard<std::mutex> lock(impl.mu);
        impl.error = "multiple iD14 MK1 devices detected; refusing to guess";
        return Result::DeviceAmbiguous;
    }

    (void)enumerate();
    const int n = getCount();
    if (n < 1)
    {
        FreeLibrary(dll);
        std::lock_guard<std::mutex> lock(impl.mu);
        impl.error = "Audient API reported no device";
        return Result::DeviceNotFound;
    }
    if (n != 1)
    {
        FreeLibrary(dll);
        std::lock_guard<std::mutex> lock(impl.mu);
        impl.error = "Audient API reported multiple devices; refusing to guess";
        return Result::DeviceAmbiguous;
    }

    std::uint32_t h = 0;
    const int openSt = openIdx(0, &h);
    if (openSt != 0 || h == 0)
    {
        FreeLibrary(dll);
        std::lock_guard<std::mutex> lock(impl.mu);
        impl.error = "TUSBAUDIO_OpenDeviceByIndex failed (status " +
                     std::to_string(openSt) + ")";
        return Result::OpenFailed;
    }

    {
        std::lock_guard<std::mutex> lock(impl.mu);
        impl.dll = dll;
        impl.enumerate = enumerate;
        impl.getDeviceCount = getCount;
        impl.openByIndex = openIdx;
        impl.closeDevice = closeDev;
        impl.requestGet = requestGet;
        impl.requestSet = requestSet;
        impl.handle = h;
        impl.error.clear();
        impl.connected.store(true, std::memory_order_relaxed);
        impl.started.store(true, std::memory_order_release);
    }
    impl.worker = std::thread(&AudientHardwareControl::pollWorker, std::ref(*this));
    return Result::Ok;
}

void AudientHardwareControl::close()
{
    Impl& impl = *impl_;
    if (!impl.started.load(std::memory_order_acquire))
    {
        return;
    }
    impl.started.store(false, std::memory_order_release);
    if (impl.worker.joinable())
    {
        impl.worker.join();
    }
    {
        std::lock_guard<std::mutex> lock(impl.mu);
        if (impl.handle != 0 && impl.closeDevice != nullptr)
        {
            (void)impl.closeDevice(impl.handle);
        }
        impl.handle = 0;
        impl.connected.store(false, std::memory_order_relaxed);
        impl.monDb.reset();
        impl.hpDb.reset();
        impl.error.clear();
        if (impl.dll != nullptr)
        {
            FreeLibrary(impl.dll);
        }
        impl.dll = nullptr;
        impl.enumerate = nullptr;
        impl.getDeviceCount = nullptr;
        impl.openByIndex = nullptr;
        impl.closeDevice = nullptr;
        impl.requestGet = nullptr;
        impl.requestSet = nullptr;
    }
}

// ---- semantic SET with read-back verify ------------------------------------

Result AudientHardwareControl::setVolumeDb(double db, bool headphones)
{
    Impl& impl = *impl_;
    if (!impl.started.load(std::memory_order_acquire))
    {
        return Result::NotOpen;
    }
    if (!detail::validWriteDb(db))
    {
        std::lock_guard<std::mutex> lock(impl.mu);
        impl.error = std::string("value ") + std::to_string(db) +
                     " dB outside the conservative SET window [" +
                     std::to_string(detail::kDbWriteFloor) + ", " +
                     std::to_string(detail::kDbWriteCeiling) + "] dB";
        return Result::InvalidDb;
    }

    const std::int16_t target = detail::dbToQ88Raw(db);
    const std::uint32_t entity = headphones ? kHpEntity : kMonEntity;
    const std::uint32_t control = headphones ? kHpVolumeControl : kMonVolumeControl;
    const std::uint32_t getArg5 = headphones ? kHpGetArg5 : kMonArg5;

    std::uint8_t payload[16]{};
    std::uint8_t scratch[16]{};
    packQ88(payload, target);

    std::unique_lock<std::mutex> lock(impl.mu);
    if (impl.handle == 0 || !impl.connected.load(std::memory_order_relaxed))
    {
        return Result::Disconnected;
    }

    int writeSt = 0;
    if (headphones)
    {
        // Official iD-equivalent 3+4 pair: two SETs, identical payload.
        const int s1 = impl.requestSet(impl.handle, entity, kSide, control,
                                       static_cast<std::uint8_t>(kHpSetArg5A), payload,
                                       kArg7Bytes, scratch, kTimeoutMs);
        const int s2 = impl.requestSet(impl.handle, entity, kSide, control,
                                       static_cast<std::uint8_t>(kHpSetArg5B), payload,
                                       kArg7Bytes, scratch, kTimeoutMs);
        writeSt = (s1 == 0 && s2 == 0) ? 0 : (s1 != 0 ? s1 : s2);
    }
    else
    {
        writeSt = impl.requestSet(impl.handle, entity, kSide, control,
                                  static_cast<std::uint8_t>(kMonArg5), payload,
                                  kArg7Bytes, scratch, kTimeoutMs);
    }

    // Read back exactly once. No retries.
    double readDb = 0.0;
    const int readSt = impl.handle != 0
                           ? readLevelLocked(impl, entity, control, getArg5, &readDb)
                           : -1;

    if (writeSt != 0)
    {
        if (readSt != 0)
        {
            // Likely the device disappeared mid-write.
            markDeviceLostLocked(impl);
            return Result::Disconnected;
        }
        impl.error = "SET returned status " + std::to_string(writeSt);
        return Result::WriteFailed;
    }
    if (readSt != 0)
    {
        if (readSt == -1)
        {
            markDeviceLostLocked(impl);
            return Result::Disconnected;
        }
        impl.error = "read-back after SET returned no plausible level";
        return Result::VerifyFailed;
    }

    const std::int32_t delta256 =
        static_cast<std::int32_t>(std::lround((readDb - db) * 256.0));
    if (delta256 < -kReadBackTol256 || delta256 > kReadBackTol256)
    {
        // Device did not land where requested - report failure, issue no more
        // writes, and leave the cache reflecting what the device actually holds.
        if (headphones)
        {
            impl.hpDb = readDb;
        }
        else
        {
            impl.monDb = readDb;
        }
        impl.error = "read-back " + std::to_string(readDb) + " dB does not match target " +
                     std::to_string(db) + " dB";
        return Result::VerifyFailed;
    }

    if (headphones)
    {
        impl.hpDb = readDb;
    }
    else
    {
        impl.monDb = readDb;
    }
    impl.error.clear();
    return Result::Ok;
}

Result AudientHardwareControl::setMonitorDb(double db)
{
    return setVolumeDb(db, false);
}

Result AudientHardwareControl::setHeadphoneDb(double db)
{
    return setVolumeDb(db, true);
}

} // namespace audient::hardware
