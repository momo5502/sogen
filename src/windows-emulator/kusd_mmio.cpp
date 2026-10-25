#include "std_include.hpp"
#include "kusd_mmio.hpp"
#include <utils/time.hpp>
#include <utils/string.hpp>
#include "windows_emulator.hpp"
#include "version/windows_version_manager.hpp"

#include <address_utils.hpp>

namespace sogen
{

    constexpr auto KUSD_ADDRESS = 0x7ffe0000ULL;
    constexpr auto KUSD_SIZE = sizeof(KUSER_SHARED_DATA64);
    constexpr auto KUSD_BUFFER_SIZE = page_align_up(KUSD_SIZE);

    namespace
    {
        void setup_kusd(KUSER_SHARED_DATA64& kusd, const windows_version_manager& version, const fake_environment_config& fake_env)
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
            kusd.RNGSeedVersion = 0;
            kusd.TimeZoneBiasStamp = 0x00000004;
            kusd.NtBuildNumber = version.get_windows_build_number();
            kusd.NtProductType = static_cast<NT_PRODUCT_TYPE>(fake_env.nt_product_type);
            kusd.ProductTypeIsValid = 0x01;
            kusd.NativeProcessorArchitecture = 0x0009;
            kusd.NtMajorVersion = version.get_major_version();
            kusd.NtMinorVersion = version.get_minor_version();
            kusd.BootId = 0;
            kusd.SystemExpirationDate.QuadPart = 0;
            kusd.SuiteMask = 0;
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
            kusd.Cookie = 0;
            kusd.ConsoleSessionForegroundProcessId = 0x00000000000028f4;
            kusd.TimeUpdateLock = 0x0000000002b28586;
            // This is the QPC time when `SystemTime` is set
            // We set it to UINT64_MAX, so `SystemTime` won't get adjusted in `RtlGetSystemTimePrecise`
            kusd.BaselineSystemTimeQpc = 0xFFFFFFFFFFFFFFFF;
            kusd.BaselineInterruptTimeQpc = 0xFFFFFFFFFFFFFFFF;
            kusd.QpcSystemTimeIncrement = 0x8000000000000000;
            kusd.QpcInterruptTimeIncrement = 0x8000000000000000;
            kusd.QpcSystemTimeIncrementShift = 0x01;
            kusd.QpcInterruptTimeIncrementShift = 0x01;
            kusd.UnparkedProcessorCount = static_cast<uint16_t>(fake_env.number_of_processors);
            kusd.TelemetryCoverageRound = 0x00000001;
            kusd.LangGenerationCount = 0x00000003;
            kusd.InterruptTimeBias = 0x00000015a5d56406;
            kusd.ActiveProcessorCount = fake_env.number_of_processors;
            kusd.ActiveGroupCount = 0x01;
            kusd.TimeZoneBiasEffectiveStart.QuadPart = 0x01db276e654cb2ff;
            kusd.TimeZoneBiasEffectiveEnd.QuadPart = 0x01db280b8c3b2800;
            kusd.XState.EnabledFeatures = 0x000000000000001f;
            kusd.XState.EnabledVolatileFeatures = 0x000000000000000f;
            kusd.XState.Size = 0x000003c0;
            kusd.QpcData.QpcData = 0x0083;
            kusd.QpcData.QpcBypassEnabled = 0x83;
            kusd.QpcBias = 0x000000159530c4af;
            kusd.QpcFrequency = utils::clock::steady_duration::period::den;
            kusd.Reserved1 = 0x7ffeffff;
            kusd.Reserved3 = 0x80000000;
            kusd.ProcessorFeatures.arr[PF_RDTSC_INSTRUCTION_AVAILABLE] = 1;
            kusd.ProcessorFeatures.arr[PF_RDTSCP_INSTRUCTION_AVAILABLE] = 1;
            kusd.ProcessorFeatures.arr[PF_RDPID_INSTRUCTION_AVAILABLE] = 0;
            // Keep these in sync with the CPUID feature set advertised by the analyzer (issue #560):
            // SSE through SSSE3 are exposed, while SSE4.x/AVX are intentionally hidden.
            kusd.ProcessorFeatures.arr[PF_COMPARE_EXCHANGE_DOUBLE] = 1;
            kusd.ProcessorFeatures.arr[PF_MMX_INSTRUCTIONS_AVAILABLE] = 1;
            kusd.ProcessorFeatures.arr[PF_XMMI_INSTRUCTIONS_AVAILABLE] = 1;
            kusd.ProcessorFeatures.arr[PF_XMMI64_INSTRUCTIONS_AVAILABLE] = 1;
            kusd.ProcessorFeatures.arr[PF_NX_ENABLED] = 1;
            kusd.ProcessorFeatures.arr[PF_SSE3_INSTRUCTIONS_AVAILABLE] = 1;
            kusd.ProcessorFeatures.arr[PF_COMPARE_EXCHANGE128] = 1;
            kusd.ProcessorFeatures.arr[PF_XSAVE_ENABLED] = 1;
            kusd.ProcessorFeatures.arr[PF_SSSE3_INSTRUCTIONS_AVAILABLE] = 1;
            kusd.ProcessorFeatures.arr[PF_FASTFAIL_AVAILABLE] = 1;

            const auto& system_root = version.get_system_root();
            utils::string::copy(kusd.NtSystemRoot.arr, std::u16string_view{system_root.u16string()});

            kusd.ImageNumberLow = IMAGE_FILE_MACHINE_AMD64;
            kusd.ImageNumberHigh = IMAGE_FILE_MACHINE_AMD64;
        }
    }

    void apply_arm64_processor(KUSER_SHARED_DATA64& kusd, const uint64_t isar0, const uint64_t isar1)
    {
        const auto field = [](const uint64_t value, const unsigned shift) { return (value >> shift) & 0xF; };

        kusd.ImageNumberLow = IMAGE_FILE_MACHINE_ARM64;
        kusd.ImageNumberHigh = IMAGE_FILE_MACHINE_ARM64;
        kusd.NativeProcessorArchitecture = PROCESSOR_ARCHITECTURE_ARM64;
        kusd.XState = {};

        auto& features = kusd.ProcessorFeatures.arr;
        std::ranges::fill(features, std::remove_cvref_t<decltype(features[0])>{});
        for (const auto feature : {PF_COMPARE_EXCHANGE_DOUBLE, PF_NX_ENABLED, PF_ARM_VFP_32_REGISTERS_AVAILABLE,
                                   PF_ARM_NEON_INSTRUCTIONS_AVAILABLE, PF_FASTFAIL_AVAILABLE, PF_ARM_DIVIDE_INSTRUCTION_AVAILABLE,
                                   PF_ARM_64BIT_LOADSTORE_ATOMIC, PF_ARM_FMAC_INSTRUCTIONS_AVAILABLE, PF_ARM_V8_INSTRUCTIONS_AVAILABLE})
        {
            features[feature] = 1;
        }

        // ID_AA64ISAR0_EL1: AES [7:4], SHA1 [11:8], SHA2 [15:12], CRC32 [19:16], Atomic [23:20], DP [47:44].
        // ID_AA64ISAR1_EL1: JSCVT [15:12], LRCPC [23:20].
        features[PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE] = field(isar0, 4) && field(isar0, 8) && field(isar0, 12);
        features[PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE] = field(isar0, 16) != 0;
        features[PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE] = field(isar0, 20) >= 2;
        features[PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE] = field(isar0, 44) != 0;
        features[PF_ARM_V83_JSCVT_INSTRUCTIONS_AVAILABLE] = field(isar1, 12) != 0;
        features[PF_ARM_V83_LRCPC_INSTRUCTIONS_AVAILABLE] = field(isar1, 20) != 0;
    }

    namespace utils
    {
        static void serialize(buffer_serializer& buffer, const KUSER_SHARED_DATA64& kusd)
        {
            static_assert(KUSD_SIZE == sizeof(kusd));
            buffer.write(&kusd, KUSD_SIZE);
        }

        static void deserialize(buffer_deserializer& buffer, KUSER_SHARED_DATA64& kusd)
        {
            buffer.read(&kusd, KUSD_SIZE);
        }
    }

    kusd_mmio::kusd_mmio(memory_manager& memory, utils::clock& clock, const bool intercept)
        : memory_(&memory),
          clock_(&clock),
          intercept_(intercept)
    {
    }

    kusd_mmio::~kusd_mmio()
    {
        this->deregister_mmio();
    }

    void kusd_mmio::setup(const windows_version_manager& version, const fake_environment_config& fake_env)
    {
        setup_kusd(this->kusd_, version, fake_env);
        this->register_mmio();
    }

    void kusd_mmio::refresh()
    {
        if (this->intercept_ || !this->registered_)
        {
            return;
        }

        const std::scoped_lock lock(this->mutex_);
        this->update();
        this->write_page();
    }

    void kusd_mmio::write_page()
    {
        // Guests read KSYSTEM_TIME without locking: High1Time, then LowPart, retrying until High2Time matches. That
        // only works if High2Time is written before LowPart and High1Time after it, but copying the page writes
        // LowPart first. Writing the new High2Time values ahead of the copy restores the order.
        const auto write_high2_time = [this](const size_t offset, const volatile KSYSTEM_TIME& time) {
            const LONG high2_time = time.High2Time;
            this->memory_->write_memory(KUSD_ADDRESS + offset + offsetof(KSYSTEM_TIME, High2Time), &high2_time, sizeof(high2_time));
        };

        write_high2_time(offsetof(KUSER_SHARED_DATA64, InterruptTime), this->kusd_.InterruptTime);
        write_high2_time(offsetof(KUSER_SHARED_DATA64, SystemTime), this->kusd_.SystemTime);
        write_high2_time(offsetof(KUSER_SHARED_DATA64, TickCount), this->kusd_.TickCount.TickCount);

        this->memory_->write_memory(KUSD_ADDRESS, &this->kusd_, KUSD_SIZE);
    }

    void kusd_mmio::serialize(utils::buffer_serializer& buffer) const
    {
        buffer.write(this->kusd_);
    }

    void kusd_mmio::deserialize(utils::buffer_deserializer& buffer)
    {
        buffer.read(this->kusd_);

        this->deregister_mmio();
        this->register_mmio();
    }

    void kusd_mmio::read(const uint64_t addr, void* data, const size_t size)
    {
        const std::scoped_lock lock(this->mutex_);

        this->update();

        if (addr >= KUSD_SIZE)
        {
            return;
        }

        const auto end = addr + size;
        const auto valid_end = std::min(end, static_cast<uint64_t>(KUSD_SIZE));
        const auto real_size = valid_end - addr;

        const auto* kusd_buffer = reinterpret_cast<uint8_t*>(&this->kusd_);
        memcpy(data, kusd_buffer + addr, static_cast<size_t>(real_size));
    }

    uint64_t kusd_mmio::address()
    {
        return KUSD_ADDRESS;
    }

    void kusd_mmio::update()
    {
        const auto time = this->clock_->system_now();
        utils::convert_to_ksystem_time(&this->kusd_.SystemTime, time);

        const auto ticks = this->clock_->steady_now();
        const auto duration_100ns =
            std::chrono::duration_cast<std::chrono::duration<uint64_t, std::ratio<1, 10000000>>>(ticks.time_since_epoch()).count();
        const auto duration_ms = duration_100ns / 10000;

        this->kusd_.TickCount.TickCountQuad = (duration_ms << 24) / this->kusd_.TickCountMultiplier;
        this->kusd_.TickCount.TickCount.High2Time = this->kusd_.TickCount.TickCount.High1Time;

        this->kusd_.InterruptTime.High2Time = static_cast<int32_t>(duration_100ns >> 32);
        this->kusd_.InterruptTime.LowPart = static_cast<uint32_t>(duration_100ns & 0xFFFFFFFF);
        this->kusd_.InterruptTime.High1Time = static_cast<int32_t>(duration_100ns >> 32);
    }

    void kusd_mmio::register_mmio()
    {
        if (this->registered_)
        {
            return;
        }

        this->registered_ = true;

        if (this->intercept_)
        {
            this->memory_->allocate_mmio(
                KUSD_ADDRESS, KUSD_BUFFER_SIZE,
                [this](const uint64_t addr, void* data, const size_t size) {
                    this->read(addr, data, size); //
                },
                [](const uint64_t, const void*, const size_t) {
                    // Writing not supported!
                });
            return;
        }

        // Classified as MMIO like the trapping variant, so the guest can't release it and snapshots leave it to
        // this object to recreate.
        this->memory_->allocate_memory(KUSD_ADDRESS, KUSD_BUFFER_SIZE, memory_permission::read, false, memory_region_kind::mmio);
        const std::scoped_lock lock(this->mutex_);
        this->update();
        this->write_page();
    }

    void kusd_mmio::deregister_mmio()
    {
        if (this->registered_)
        {
            this->registered_ = false;
            this->memory_->release_memory(KUSD_ADDRESS, KUSD_BUFFER_SIZE);
        }
    }

} // namespace sogen
