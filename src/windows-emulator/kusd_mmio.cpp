#include "kusd_mmio.hpp"
#include <utils/time.hpp>
#include "windows_emulator.hpp"

#include <address_utils.hpp>

constexpr auto KUSD_ADDRESS = 0x7ffe0000ULL;
constexpr auto KUSD_SIZE = sizeof(KUSER_SHARED_DATA64);
constexpr auto KUSD_BUFFER_SIZE = page_align_up(KUSD_SIZE);

namespace
{
    void setup_kusd(KUSER_SHARED_DATA64& kusd, const bool use_relative_time)
    {
        memset(reinterpret_cast<void*>(&kusd), 0, sizeof(kusd));

        kusd.TickCountMultiplier = 0x0fa00000;
        kusd.InterruptTime.LowPart = 0x17bd9547;
        kusd.InterruptTime.High1Time = 0x0000004b;
        kusd.InterruptTime.High2Time = 0x0000004b;
        kusd.SystemTime.LowPart = 0x7af9da99;
        kusd.SystemTime.High1Time = 0x01db27b9;
        kusd.SystemTime.High2Time = 0x01db27b9;
        kusd.TimeZoneBias.LowPart = 0x3c773000;
        kusd.TimeZoneBias.High1Time = -17;
        kusd.TimeZoneBias.High2Time = -17;
        kusd.TimeZoneId = 0x00000002;
        kusd.LargePageMinimum = 0x00200000;
        kusd.RNGSeedVersion = 0x0000000000000013;
        kusd.TimeZoneBiasStamp = 0x00000004;
        kusd.NtBuildNumber = 0x00006c51;
        kusd.NtProductType = NtProductWinNt;
        kusd.ProductTypeIsValid = 0x01;
        kusd.NativeProcessorArchitecture = 0x0009;
        kusd.NtMajorVersion = 0x0000000a;
        kusd.BootId = 0x0000000b;
        kusd.SystemExpirationDate.QuadPart = 0x01dc26860a9ff300;
        kusd.SuiteMask = 0x00000110;
        kusd.MitigationPolicies.MitigationPolicies = 0x0a;
        kusd.MitigationPolicies.NXSupportPolicy = 0x02;
        kusd.MitigationPolicies.SEHValidationPolicy = 0x02;
        kusd.CyclesPerYield = 0x0064;
        kusd.DismountCount = 0x00000006;
        kusd.ComPlusPackage = 0x00000001;
        kusd.LastSystemRITEventTickCount = 0x01ec1fd3;
        kusd.NumberOfPhysicalPages = 0x00bf0958;
        kusd.FullNumberOfPhysicalPages = 0x0000000000bf0958;
        kusd.TickCount.TickCount.LowPart = 0x001f7f05;
        kusd.TickCount.TickCountQuad = 0x00000000001f7f05;
        kusd.Cookie = 0x1c3471da;
        kusd.ConsoleSessionForegroundProcessId = 0x00000000000028f4;
        kusd.TimeUpdateLock = 0x0000000002b28586;
        kusd.BaselineSystemTimeQpc = 0x0000004b17cd596c;
        kusd.BaselineInterruptTimeQpc = 0x0000004b17cd596c;
        kusd.QpcSystemTimeIncrement = 0x8000000000000000;
        kusd.QpcInterruptTimeIncrement = 0x8000000000000000;
        kusd.QpcSystemTimeIncrementShift = 0x01;
        kusd.QpcInterruptTimeIncrementShift = 0x01;
        kusd.UnparkedProcessorCount = 0x000c;
        kusd.TelemetryCoverageRound = 0x00000001;
        kusd.LangGenerationCount = 0x00000003;
        kusd.InterruptTimeBias = 0x00000015a5d56406;
        kusd.QpcBias = 0x000000159530c4af;
        kusd.ActiveProcessorCount = 0x0000000c;
        kusd.ActiveGroupCount = 0x01;
        kusd.QpcData.QpcData = 0x0083;
        kusd.QpcData.QpcBypassEnabled = 0x83;
        kusd.TimeZoneBiasEffectiveStart.QuadPart = 0x01db276e654cb2ff;
        kusd.TimeZoneBiasEffectiveEnd.QuadPart = 0x01db280b8c3b2800;
        kusd.XState.EnabledFeatures = 0x000000000000001f;
        kusd.XState.EnabledVolatileFeatures = 0x000000000000000f;
        kusd.XState.Size = 0x000003c0;

        if (use_relative_time)
        {
            kusd.QpcFrequency = 1000;
        }
        else
        {
            kusd.QpcFrequency = std::chrono::steady_clock::period::den;
        }

        constexpr std::wstring_view root_dir{L"C:\\WINDOWS"};
        memcpy(&kusd.NtSystemRoot.arr[0], root_dir.data(), root_dir.size() * 2);

        kusd.ImageNumberLow = IMAGE_FILE_MACHINE_I386;
        kusd.ImageNumberHigh = IMAGE_FILE_MACHINE_AMD64;
    }
}

namespace utils
{
    inline void serialize(buffer_serializer& buffer, const KUSER_SHARED_DATA64& kusd)
    {
        static_assert(KUSD_SIZE == sizeof(kusd));
        buffer.write(&kusd, KUSD_SIZE);
    }

    inline void deserialize(buffer_deserializer& buffer, KUSER_SHARED_DATA64& kusd)
    {
        buffer.read(&kusd, KUSD_SIZE);
    }
}

kusd_mmio::kusd_mmio(x64_emulator& emu, process_context& process)
    : emu_(&emu),
      process_(&process)
{
}

kusd_mmio::~kusd_mmio()
{
    this->deregister_mmio();
}

kusd_mmio::kusd_mmio(utils::buffer_deserializer& buffer)
    : kusd_mmio(buffer.read<x64_emulator_wrapper>(), buffer.read<process_context_wrapper>())
{
}

void kusd_mmio::setup(const bool use_relative_time)
{
    this->use_relative_time_ = use_relative_time;

    setup_kusd(this->kusd_, use_relative_time);
    this->start_time_ = utils::convert_from_ksystem_time(this->kusd_.SystemTime);

    this->register_mmio();
}

void kusd_mmio::serialize(utils::buffer_serializer& buffer) const
{
    buffer.write(this->use_relative_time_);
    buffer.write(this->kusd_);
    buffer.write(this->start_time_);
}

void kusd_mmio::deserialize(utils::buffer_deserializer& buffer)
{
    buffer.read(this->use_relative_time_);
    buffer.read(this->kusd_);
    buffer.read(this->start_time_);

    this->deregister_mmio();
    this->register_mmio();
}

uint64_t kusd_mmio::read(const uint64_t addr, const size_t size)
{
    uint64_t result{};

    this->update();

    if (addr >= KUSD_SIZE)
    {
        return result;
    }

    const auto end = addr + size;
    const auto valid_end = std::min(end, static_cast<uint64_t>(KUSD_SIZE));
    const auto real_size = valid_end - addr;

    if (real_size > sizeof(result))
    {
        return result;
    }

    const auto* kusd_buffer = reinterpret_cast<uint8_t*>(&this->kusd_);
    memcpy(&result, kusd_buffer + addr, real_size);

    return result;
}

uint64_t kusd_mmio::address()
{
    return KUSD_ADDRESS;
}

void kusd_mmio::update()
{
    auto time = this->start_time_;

    if (this->use_relative_time_)
    {
        const auto passed_time = this->process_->executed_instructions;
        const auto clock_frequency = static_cast<uint64_t>(this->kusd_.QpcFrequency);

        using duration = std::chrono::system_clock::duration;
        time += duration(passed_time * duration::period::den / clock_frequency);
    }
    else
    {
        time = std::chrono::system_clock::now();
    }

    utils::convert_to_ksystem_time(&this->kusd_.SystemTime, time);
}

void kusd_mmio::register_mmio()
{
    if (this->registered_)
    {
        return;
    }

    this->registered_ = true;

    this->emu_->allocate_mmio(
        KUSD_ADDRESS, KUSD_BUFFER_SIZE,
        [this](const uint64_t addr, const size_t size) { return this->read(addr, size); },
        [](const uint64_t, const size_t, const uint64_t) {
            // Writing not supported!
        });
}

void kusd_mmio::deregister_mmio()
{
    if (this->registered_)
    {
        this->registered_ = false;
        this->emu_->release_memory(KUSD_ADDRESS, KUSD_BUFFER_SIZE);
    }
}
