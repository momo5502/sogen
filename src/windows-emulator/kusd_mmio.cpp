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

    // The hypervisor's reference TSC page (HV_REFERENCE_TSC_PAGE), which Windows maps right after the KUSD in every process.
    struct reference_tsc_page_layout
    {
        uint32_t tsc_sequence;
        uint32_t reserved;
        uint64_t tsc_scale;
        int64_t tsc_offset;
    };

    constexpr auto REFERENCE_TSC_PAGE_ADDRESS = KUSD_ADDRESS + KUSD_BUFFER_SIZE;
    constexpr auto REFERENCE_TSC_PAGE_SIZE = page_align_up(sizeof(reference_tsc_page_layout));
    constexpr uint64_t REFERENCE_TIME_FREQUENCY = HUNDRED_NANOSECONDS_IN_ONE_SECOND;

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
            kusd.NumberOfPhysicalPages = simulated_physical_page_count;
            kusd.FullNumberOfPhysicalPages = simulated_physical_page_count;
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

    uint64_t reference_tsc_scale(const uint64_t tsc_frequency)
    {
        // Long division of 10^7 * 2^64, one quotient bit at a time; it fits in 64 bits because 10^7 < tsc_frequency.
        uint64_t remainder = REFERENCE_TIME_FREQUENCY;
        uint64_t quotient = 0;
        for (int bit = 0; bit < 64; ++bit)
        {
            const bool carry = (remainder >> 63) != 0;
            remainder <<= 1;
            quotient <<= 1;
            if (carry || remainder >= tsc_frequency)
            {
                remainder -= tsc_frequency;
                quotient |= 1;
            }
        }

        return quotient;
    }

    uint64_t reference_time(const uint64_t tsc, const uint64_t tsc_scale)
    {
        // The high half of the 128-bit product, built from 32-bit halves.
        const uint64_t tsc_low = static_cast<uint32_t>(tsc);
        const uint64_t tsc_high = tsc >> 32;
        const uint64_t scale_low = static_cast<uint32_t>(tsc_scale);
        const uint64_t scale_high = tsc_scale >> 32;

        const uint64_t low_low = tsc_low * scale_low;
        const uint64_t high_low = tsc_high * scale_low;
        const uint64_t low_high = tsc_low * scale_high;
        const uint64_t middle = (low_low >> 32) + static_cast<uint32_t>(high_low) + low_high;

        return tsc_high * scale_high + (high_low >> 32) + (middle >> 32);
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

    kusd_mmio::kusd_mmio(memory_manager& memory, utils::clock& clock, const bool intercept,
                         std::function<std::optional<native_tsc>()> read_tsc)
        : memory_(&memory),
          clock_(&clock),
          intercept_(intercept),
          read_tsc_(std::move(read_tsc))
    {
    }

    kusd_mmio::~kusd_mmio()
    {
        this->deregister_mmio();
    }

    void kusd_mmio::setup(const windows_version_manager& version, const fake_environment_config& fake_env,
                          const std::optional<uint64_t> tsc_frequency)
    {
        setup_kusd(this->kusd_, version, fake_env);

        // QpcBypassEnabled already asks ntdll for the hypervisor page; once there is one to read, QPC needs no syscall.
        // Its reference time counts 100 ns units from the partition's first run, like a freshly booted Hyper-V guest.
        if (tsc_frequency && *tsc_frequency > REFERENCE_TIME_FREQUENCY)
        {
            this->reference_tsc_scale_ = reference_tsc_scale(*tsc_frequency);
            this->reference_tsc_sequence_ = 1;
            this->kusd_.QpcFrequency = HUNDRED_NANOSECONDS_IN_ONE_SECOND;
            this->kusd_.QpcBias = 0;
        }

        this->register_mmio();
    }

    std::optional<uint64_t> kusd_mmio::reference_tsc_page() const
    {
        if (!this->reference_tsc_scale_)
        {
            return std::nullopt;
        }

        return REFERENCE_TSC_PAGE_ADDRESS;
    }

    uint64_t kusd_mmio::query_performance_counter(const uint64_t tsc) const
    {
        // ntdll: ((tsc * TscScale) >> 64) + TscOffset + QpcBias, shifted right by QpcShift, which is 0.
        const auto bias = this->access([](const KUSER_SHARED_DATA64& kusd) { return kusd.QpcBias; });
        return reference_time(tsc, this->reference_tsc_scale_) + static_cast<uint64_t>(this->reference_tsc_offset_) + bias;
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
        buffer.write(this->reference_tsc_scale_);
        buffer.write(this->reference_tsc_sequence_);
        buffer.write(this->reference_tsc_scale_ ? reference_time(this->read_native_tsc().value, this->reference_tsc_scale_) +
                                                      static_cast<uint64_t>(this->reference_tsc_offset_)
                                                : uint64_t{0});
    }

    void kusd_mmio::deserialize(utils::buffer_deserializer& buffer)
    {
        // Releases the page of the scale in use until now.
        this->deregister_mmio();

        buffer.read(this->kusd_);
        buffer.read(this->reference_tsc_scale_);
        buffer.read(this->reference_tsc_sequence_);
        const auto saved_reference_time = buffer.read<uint64_t>();

        // ntdll keeps reading the page it was given at process start, and neither the new partition's TSC nor, on another
        // machine, its rate continue the saved ones. As Hyper-V does when it rewrites the page after a restore or a
        // migration, TscScale follows this host's TSC rate, TscOffset moves so the reference time goes on from the saved
        // value, and the sequence changes so a read that the save interrupted starts over (0 would mean no page).
        this->reference_tsc_offset_ = 0;
        if (this->reference_tsc_scale_)
        {
            const auto tsc = this->read_native_tsc();
            this->reference_tsc_scale_ = reference_tsc_scale(tsc.frequency);
            this->reference_tsc_offset_ =
                static_cast<int64_t>(saved_reference_time - reference_time(tsc.value, this->reference_tsc_scale_));
            this->reference_tsc_sequence_ = this->reference_tsc_sequence_ == UINT32_MAX ? 1 : this->reference_tsc_sequence_ + 1;
        }

        this->register_mmio();
    }

    native_tsc kusd_mmio::read_native_tsc() const
    {
        // The page exists only for a guest that reads its TSC natively; one restored elsewhere would mix the two QPC clocks.
        const auto tsc = this->read_tsc_ ? this->read_tsc_() : std::nullopt;
        if (!tsc || tsc->frequency <= REFERENCE_TIME_FREQUENCY)
        {
            throw std::runtime_error("The reference TSC page needs a TSC the guest reads natively");
        }

        return *tsc;
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

        // Same ordering problem write_page describes: a copy writes LowPart before High1Time, so the new High2Time
        // has to reach the destination first. It matters when the destination is a page a running vCPU reads.
        const auto write_high2_time = [&](const size_t offset, const volatile KSYSTEM_TIME& time) {
            const auto field = offset + offsetof(KSYSTEM_TIME, High2Time);
            if (field < addr || field + sizeof(LONG) > valid_end)
            {
                return;
            }

            const LONG high2_time = time.High2Time;
            memcpy(static_cast<uint8_t*>(data) + (field - addr), &high2_time, sizeof(high2_time));
        };

        write_high2_time(offsetof(KUSER_SHARED_DATA64, InterruptTime), this->kusd_.InterruptTime);
        write_high2_time(offsetof(KUSER_SHARED_DATA64, SystemTime), this->kusd_.SystemTime);
        write_high2_time(offsetof(KUSER_SHARED_DATA64, TickCount), this->kusd_.TickCount.TickCount);

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
        this->memory_->register_fixed_guest_range(KUSD_ADDRESS, KUSD_BUFFER_SIZE);
        this->map_reference_tsc_page();

        if (this->intercept_)
        {
            const bool mapped = this->memory_->allocate_mmio(
                KUSD_ADDRESS, KUSD_BUFFER_SIZE,
                [this](const uint64_t addr, void* data, const size_t size) {
                    this->read(addr, data, size); //
                },
                [](const uint64_t, const void*, const size_t) {
                    // Writing not supported!
                });
            if (!mapped)
            {
                throw std::runtime_error("Failed to map KUSER_SHARED_DATA");
            }
            return;
        }

        // Classified as MMIO like the trapping variant, so the guest can't release it and snapshots leave it to
        // this object to recreate.
        if (!this->memory_->allocate_memory(KUSD_ADDRESS, KUSD_BUFFER_SIZE, memory_permission::read, false, memory_region_kind::mmio))
        {
            throw std::runtime_error("Failed to map KUSER_SHARED_DATA");
        }
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
            if (this->reference_tsc_scale_)
            {
                this->memory_->release_memory(REFERENCE_TSC_PAGE_ADDRESS, REFERENCE_TSC_PAGE_SIZE);
            }
        }
    }

    void kusd_mmio::map_reference_tsc_page()
    {
        if (!this->reference_tsc_scale_)
        {
            return;
        }

        this->memory_->register_fixed_guest_range(REFERENCE_TSC_PAGE_ADDRESS, REFERENCE_TSC_PAGE_SIZE);
        if (!this->memory_->allocate_memory(REFERENCE_TSC_PAGE_ADDRESS, REFERENCE_TSC_PAGE_SIZE, memory_permission::read, false,
                                            memory_region_kind::mmio))
        {
            throw std::runtime_error("Failed to map the reference TSC page");
        }

        // A TSC of fixed rate needs no rewrite while the process runs; only a restore moves the offset and the sequence.
        const reference_tsc_page_layout page{.tsc_sequence = this->reference_tsc_sequence_,
                                             .reserved = 0,
                                             .tsc_scale = this->reference_tsc_scale_,
                                             .tsc_offset = this->reference_tsc_offset_};
        this->memory_->write_memory(REFERENCE_TSC_PAGE_ADDRESS, &page, sizeof(page));
    }

} // namespace sogen
