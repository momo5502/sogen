#include "std_include.hpp"

namespace sogen::syscalls
{
    NTSTATUS handle_NtGdiInit(const syscall_context& c);
    uint64_t handle_NtGdiCreateCompatibleDC(const syscall_context& c, hdc dc);
    uint64_t handle_NtGdiCreateBitmap(const syscall_context& c, uint32_t width, uint32_t height, uint32_t planes, uint32_t bits_pixel,
                                      emulator_pointer bits);
    uint64_t handle_NtGdiSelectBitmap(const syscall_context& c, hdc dc, handle bitmap);
    uint32_t handle_NtGdiDeleteObjectApp(const syscall_context& c, uint32_t handle_value);
    int32_t handle_NtGdiSaveDC(const syscall_context& c, hdc dc);
    BOOL handle_NtGdiRestoreDC(const syscall_context& c, hdc dc, int32_t saved_dc);
    BOOL handle_NtGdiFlush(const syscall_context& c);
    uint32_t handle_NtGdiSetBoundsRect(const syscall_context& c, hdc dc, emulator_pointer rect, uint32_t flags);
    uint32_t handle_NtGdiGetBoundsRect(const syscall_context& c, hdc dc, emulator_pointer rect, uint32_t flags);
    BOOL handle_NtGdiRectangle(const syscall_context& c, hdc dc, LONG left, LONG top, LONG right, LONG bottom);
    BOOL handle_NtGdiPatBlt(const syscall_context& c, hdc dc, LONG x, LONG y, LONG width, LONG height, DWORD rop);
    BOOL handle_NtGdiBitBlt(const syscall_context& c, hdc dst_dc, int x_dst, int y_dst, int width, int height, hdc src_dc, int x_src,
                            int y_src, DWORD rop, DWORD cr_back_color, FLONG fl);
    BOOL handle_NtGdiStretchBlt(const syscall_context& c, hdc dst_dc, int x_dst, int y_dst, int dst_width, int dst_height, hdc src_dc,
                                int x_src, int y_src, int src_width, int src_height, DWORD rop, DWORD cr_back_color);
    BOOL handle_NtGdiLineTo(const syscall_context& c, hdc dc, LONG x_end, LONG y_end);
    COLORREF handle_NtGdiSetPixel(const syscall_context& c, hdc dc, int x, int y, COLORREF color);
}

namespace sogen::test
{
    namespace
    {
        constexpr uint32_t k_dcb_reset = 0x1;
        constexpr uint32_t k_dcb_accumulate = 0x2;
        constexpr uint32_t k_dcb_enable = 0x4;
        constexpr uint32_t k_dcb_disable = 0x8;
        constexpr uint32_t k_dcb_windowmgr = 0x8000;

        constexpr int k_side = 64;
        constexpr uint64_t k_page = 0x1000;
        constexpr uint64_t k_dc_attr_pt_current = 0xD8;
        constexpr uint64_t k_kernel_pointer = 0xFFFF800000001000ULL;
        constexpr DWORD k_srccopy = 0x00CC0020;
        constexpr DWORD k_patcopy = 0x00F00021;
        constexpr DWORD k_untouched_last_error = 0xDEAD;
        constexpr int16_t k_batch_text_out_rect = 3;
        constexpr UINT k_eto_opaque = 0x2;

        struct batch_text_out_rect
        {
            int16_t size{sizeof(batch_text_out_rect)};
            int16_t command{k_batch_text_out_rect};
            COLORREF background{};
            UINT options{};
            RECT rect{};
            POINTL viewport_org{};
            uint32_t background_attribute{};
        };

        using box = std::array<LONG, 4>;

        constexpr LONG k_prefill = static_cast<LONG>(0xCCCCCCCCu);
        constexpr box k_untouched{k_prefill, k_prefill, k_prefill, k_prefill};
        constexpr box k_empty{0, 0, 0, 0};
        constexpr box k_rect_a{10, 20, 30, 40};
        constexpr box k_rect_b{50, 60, 70, 80};
        constexpr box k_rect_c{100, 100, 110, 110};
        constexpr box k_rect_inside{35, 36, 45, 46};

        enum class operation
        {
            set_bounds,
            get_bounds,
            draw_rectangle,
            save_dc,
            restore_dc,
        };

        struct step
        {
            operation kind{};
            uint32_t flags{};
            std::optional<box> rect{};
            uint32_t expected_state{};
            box expected_rect{};
        };

        step set(const uint32_t flags, const std::optional<box>& rect, const uint32_t expected_state)
        {
            return {.kind = operation::set_bounds, .flags = flags, .rect = rect, .expected_state = expected_state, .expected_rect = {}};
        }

        step get(const uint32_t flags, const uint32_t expected_state, const box& expected_rect)
        {
            return {.kind = operation::get_bounds,
                    .flags = flags,
                    .rect = std::nullopt,
                    .expected_state = expected_state,
                    .expected_rect = expected_rect};
        }

        step draw_rectangle(const box& area)
        {
            return {.kind = operation::draw_rectangle, .flags = 0, .rect = area, .expected_state = 0, .expected_rect = {}};
        }

        step save_dc(const uint32_t expected_index)
        {
            return {.kind = operation::save_dc, .flags = 0, .rect = std::nullopt, .expected_state = expected_index, .expected_rect = {}};
        }

        step restore_dc(const int32_t index)
        {
            return {.kind = operation::restore_dc,
                    .flags = static_cast<uint32_t>(index),
                    .rect = std::nullopt,
                    .expected_state = 0,
                    .expected_rect = {}};
        }

        enum class primitive
        {
            rectangle,
            pat_blt,
            bit_blt,
            stretch_blt,
            line_to,
            set_pixel,
        };

        struct state_case
        {
            std::string_view name{};
            std::vector<step> steps{};
        };

        class GdiBoundsRectTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                this->emu.setup_process_if_necessary();
                syscalls::handle_NtGdiInit(this->context());
                this->scratch = this->emu.memory.allocate_memory(k_page, memory_permission::read_write);
            }

            syscall_context context()
            {
                return {.win_emu = this->emu, .emu = this->emu.vcpu(0).cpu, .vcpu = this->emu.vcpu(0), .proc = this->emu.process};
            }

            hdc create_dc(const uint32_t side = k_side)
            {
                const auto dc = static_cast<hdc>(syscalls::handle_NtGdiCreateCompatibleDC(this->context(), 0));
                const auto bitmap = syscalls::handle_NtGdiCreateBitmap(this->context(), side, side, 1, 32, 0);
                EXPECT_NE(0u, dc);
                EXPECT_NE(0u, bitmap);
                syscalls::handle_NtGdiSelectBitmap(this->context(), dc, make_handle(bitmap));
                return dc;
            }

            uint64_t dc_attr(const hdc dc)
            {
                const auto table = this->emu.process.peb64.read().GdiSharedHandleTable;
                const auto index = static_cast<uint32_t>(dc) & 0xFFFF;
                return this->emu.memory.read_memory<GDI_HANDLE_ENTRY64>(table + (index * sizeof(GDI_HANDLE_ENTRY64))).Object;
            }

            void reset_last_error()
            {
                this->emu.vcpu(0).active_thread->teb64->access([](TEB64& teb) { teb.LastErrorValue = k_untouched_last_error; });
            }

            DWORD last_error()
            {
                return this->emu.vcpu(0).active_thread->teb64->read().LastErrorValue;
            }

            uint32_t set_bounds(const hdc dc, const std::optional<box>& rect, const uint32_t flags)
            {
                if (rect)
                {
                    this->emu.memory.write_memory(this->scratch, rect->data(), sizeof(*rect));
                }
                return this->set_bounds_at(dc, rect ? this->scratch : 0, flags);
            }

            uint32_t set_bounds_at(const hdc dc, const uint64_t rect, const uint32_t flags)
            {
                this->reset_last_error();
                const auto state = syscalls::handle_NtGdiSetBoundsRect(this->context(), dc, rect, flags);
                EXPECT_EQ(k_untouched_last_error, this->last_error());
                return state;
            }

            uint32_t get_bounds(const hdc dc, const uint32_t flags, box& rect)
            {
                return this->get_bounds_at(dc, this->scratch + 0x100, flags, &rect);
            }

            uint32_t get_bounds_at(const hdc dc, const uint64_t rect_address, const uint32_t flags, box* rect = nullptr)
            {
                if (rect != nullptr)
                {
                    this->emu.memory.write_memory(rect_address, k_untouched.data(), sizeof(k_untouched));
                }

                this->reset_last_error();
                const auto state = syscalls::handle_NtGdiGetBoundsRect(this->context(), dc, rect_address, flags);
                EXPECT_EQ(k_untouched_last_error, this->last_error());
                if (rect != nullptr)
                {
                    this->emu.memory.read_memory(rect_address, rect->data(), sizeof(*rect));
                }
                return state;
            }

            void expect_bounds(const hdc dc, const uint32_t expected_state, const box& expected_rect)
            {
                box rect{};
                EXPECT_EQ(expected_state, this->get_bounds(dc, 0, rect));
                EXPECT_EQ(expected_rect, rect);
            }

            void draw(const primitive kind, const hdc dc, const hdc source, const box& area)
            {
                const auto c = this->context();
                const auto width = area[2] - area[0];
                const auto height = area[3] - area[1];
                switch (kind)
                {
                case primitive::rectangle:
                    syscalls::handle_NtGdiRectangle(c, dc, area[0], area[1], area[2], area[3]);
                    break;
                case primitive::pat_blt:
                    syscalls::handle_NtGdiPatBlt(c, dc, area[0], area[1], width, height, k_patcopy);
                    break;
                case primitive::bit_blt:
                    syscalls::handle_NtGdiBitBlt(c, dc, area[0], area[1], width, height, source, 0, 0, k_srccopy, 0, 0);
                    break;
                case primitive::stretch_blt:
                    syscalls::handle_NtGdiStretchBlt(c, dc, area[0], area[1], width, height, source, 0, 0, 8, 8, k_srccopy, 0);
                    break;
                case primitive::line_to: {
                    const std::array<LONG, 2> start{area[0], area[1]};
                    this->emu.memory.write_memory(this->dc_attr(dc) + k_dc_attr_pt_current, start.data(), sizeof(start));
                    syscalls::handle_NtGdiLineTo(c, dc, area[2], area[3]);
                    break;
                }
                case primitive::set_pixel:
                    syscalls::handle_NtGdiSetPixel(c, dc, area[0], area[1], 0xFF0000);
                    break;
                }
            }

            void run(const hdc dc, const state_case& test_case)
            {
                SCOPED_TRACE(test_case.name);
                int index = 0;
                for (const auto& s : test_case.steps)
                {
                    SCOPED_TRACE(index++);
                    switch (s.kind)
                    {
                    case operation::set_bounds:
                        EXPECT_EQ(s.expected_state, this->set_bounds(dc, s.rect, s.flags));
                        break;
                    case operation::get_bounds: {
                        box rect{};
                        EXPECT_EQ(s.expected_state, this->get_bounds(dc, s.flags, rect));
                        EXPECT_EQ(s.expected_rect, rect);
                        break;
                    }
                    case operation::draw_rectangle:
                        this->draw(primitive::rectangle, dc, dc, *s.rect);
                        break;
                    case operation::save_dc:
                        EXPECT_EQ(static_cast<int32_t>(s.expected_state), syscalls::handle_NtGdiSaveDC(this->context(), dc));
                        break;
                    case operation::restore_dc:
                        EXPECT_EQ(TRUE, syscalls::handle_NtGdiRestoreDC(this->context(), dc, static_cast<int32_t>(s.flags)));
                        break;
                    }
                }
            }

            void expect_invalid_handles_fail()
            {
                const auto good = this->create_dc();
                const auto deleted = this->create_dc();
                syscalls::handle_NtGdiDeleteObjectApp(this->context(), static_cast<uint32_t>(deleted));
                const auto bitmap = syscalls::handle_NtGdiCreateBitmap(this->context(), 8, 8, 1, 32, 0);

                const std::array<std::pair<std::string_view, hdc>, 7> invalid_handles{{
                    {"null", 0},
                    {"garbage_0x1234", 0x1234},
                    {"garbage_0xDEADBEEF", 0xDEADBEEFu},
                    {"deleted_mem_dc", deleted},
                    {"bitmap_handle", static_cast<hdc>(bitmap)},
                    {"valid_dc_wrong_unique", good ^ 0x00010000u},
                    {"valid_dc_plus_1", good + 1},
                }};

                for (const auto& [name, dc] : invalid_handles)
                {
                    SCOPED_TRACE(name);
                    EXPECT_EQ(0u, this->set_bounds(dc, box{1, 2, 3, 4}, k_dcb_enable));
                    EXPECT_EQ(0u, this->set_bounds(dc, box{1, 2, 3, 4}, k_dcb_accumulate));
                    EXPECT_EQ(0u, this->set_bounds(dc, std::nullopt, 0));

                    box rect{};
                    EXPECT_EQ(0u, this->get_bounds(dc, 0, rect));
                    EXPECT_EQ(k_untouched, rect);
                    EXPECT_EQ(0u, this->get_bounds(dc, k_dcb_reset, rect));
                    EXPECT_EQ(k_untouched, rect);
                }

                EXPECT_EQ(0x9u, this->set_bounds(good, box{1, 2, 3, 4}, k_dcb_enable));
            }

            void expect_unusable_rect_pointers_fail()
            {
                const auto read_only = this->emu.memory.allocate_memory(k_page, memory_permission::read);
                const auto no_access = this->emu.memory.allocate_memory(k_page, memory_permission::none);
                const auto two_pages = this->emu.memory.allocate_memory(2 * k_page, memory_permission::read_write);
                ASSERT_TRUE(this->emu.memory.protect_memory(two_pages + k_page, k_page, memory_permission::none));

                struct pointer_case
                {
                    std::string_view name{};
                    uint64_t address{};
                    bool usable_for_set{};
                };

                const std::array<pointer_case, 6> pointers{{
                    {.name = "null", .address = 0, .usable_for_set = true},
                    {.name = "low0x10", .address = 0x10, .usable_for_set = false},
                    {.name = "kernel", .address = k_kernel_pointer, .usable_for_set = false},
                    {.name = "noaccess", .address = no_access, .usable_for_set = false},
                    {.name = "readonly", .address = read_only, .usable_for_set = true},
                    {.name = "straddle_noaccess_tail", .address = two_pages + k_page - 8, .usable_for_set = false},
                }};

                for (const auto& pointer : pointers)
                {
                    SCOPED_TRACE(pointer.name);
                    const auto prepare = [&] {
                        const auto dc = this->create_dc();
                        EXPECT_EQ(0x9u, this->set_bounds(dc, std::nullopt, k_dcb_enable));
                        EXPECT_EQ(0x5u, this->set_bounds(dc, k_rect_a, k_dcb_accumulate));
                        return dc;
                    };

                    auto dc = prepare();
                    EXPECT_EQ(0u, this->get_bounds_at(dc, pointer.address, 0));
                    this->expect_bounds(dc, 0x3, {0, 0, 30, 40});

                    dc = prepare();
                    EXPECT_EQ(0u, this->get_bounds_at(dc, pointer.address, k_dcb_reset));
                    this->expect_bounds(dc, 0x1, k_empty);

                    dc = prepare();
                    EXPECT_EQ(pointer.usable_for_set ? 0x7u : 0u, this->set_bounds_at(dc, pointer.address, k_dcb_accumulate));
                    this->expect_bounds(dc, 0x3, {0, 0, 30, 40});

                    dc = prepare();
                    EXPECT_EQ(pointer.usable_for_set ? 0x7u : 0u, this->set_bounds_at(dc, pointer.address, k_dcb_enable));
                }
            }

            void queue_opaque_rectangle(const hdc dc, const RECT& rect)
            {
                const batch_text_out_rect record{.size = sizeof(batch_text_out_rect),
                                                 .command = k_batch_text_out_rect,
                                                 .background = 0,
                                                 .options = k_eto_opaque,
                                                 .rect = rect,
                                                 .viewport_org = {},
                                                 .background_attribute = 0};
                this->emu.vcpu(0).active_thread->teb64->access([&](TEB64& teb) {
                    auto& batch = teb.GdiTebBatch;
                    std::memcpy(reinterpret_cast<std::byte*>(batch.Buffer) + batch.Offset, &record, sizeof(record));
                    batch.Offset += sizeof(record);
                    batch.HDC = static_cast<uint64_t>(dc);
                    ++teb.GdiBatchCount;
                });
            }

            windows_emulator emu = create_sample_emulator();
            uint64_t scratch{};
        };

        std::vector<state_case> flag_and_rectangle_cases()
        {
            return {
                {"default_mem64",
                 {get(0, 0x1, k_empty), get(k_dcb_reset, 0x1, k_empty), get(0, 0x1, k_empty), set(0, std::nullopt, 0x9),
                  get(0, 0x1, k_empty)}},
                {"flags_set_0x2_pre0",
                 {set(k_dcb_accumulate, k_rect_a, 0x9), get(0, 0x3, {0, 0, 30, 40}), set(k_dcb_accumulate, k_rect_b, 0xb),
                  get(0, 0x3, {0, 0, 64, 64}), set(k_dcb_accumulate, std::nullopt, 0xb), get(k_dcb_reset, 0x3, {0, 0, 64, 64}),
                  get(0, 0x1, k_empty)}},
                {"flags_set_0x3_pre2",
                 {set(k_dcb_enable, std::nullopt, 0x9), set(k_dcb_accumulate, k_rect_c, 0x5), get(0, 0x3, {0, 0, 64, 64}),
                  set(0x3, k_rect_a, 0x7), get(0, 0x3, k_rect_a), set(0x3, k_rect_b, 0x7), get(0, 0x3, {50, 60, 64, 64}),
                  set(0x3, std::nullopt, 0x7), get(0, 0x1, k_empty), get(k_dcb_reset, 0x1, k_empty), get(0, 0x1, k_empty)}},
                {"flags_set_0x1_pre2",
                 {set(k_dcb_enable, std::nullopt, 0x9), set(k_dcb_accumulate, k_rect_c, 0x5), set(k_dcb_reset, k_rect_a, 0x7),
                  get(0, 0x1, k_empty), set(k_dcb_reset, k_rect_b, 0x5)}},
                {"flags_set_0x4_and_0x5_pre0",
                 {set(k_dcb_enable, k_rect_a, 0x9), get(0, 0x1, k_empty), set(0x5, k_rect_b, 0x5), set(0, std::nullopt, 0x5)}},
                {"flags_set_0x8_pre1",
                 {set(k_dcb_enable, std::nullopt, 0x9), set(k_dcb_disable, k_rect_a, 0x5), set(k_dcb_disable, k_rect_b, 0x9)}},
                {"flags_set_0xc_pre1",
                 {set(k_dcb_enable, std::nullopt, 0x9), set(0xc, k_rect_a, 0x5), get(0, 0x1, k_empty), set(0xc, k_rect_b, 0x9)}},
                {"flags_set_0x9_pre1", {set(k_dcb_enable, std::nullopt, 0x9), set(0x9, k_rect_a, 0x5), set(0x9, k_rect_b, 0x9)}},
                {"flags_set_0x6_pre0",
                 {set(0x6, k_rect_a, 0x9), get(0, 0x3, {0, 0, 30, 40}), set(0x6, k_rect_b, 0x7), get(0, 0x3, {0, 0, 64, 64})}},
                {"flags_set_0xe_pre2",
                 {set(k_dcb_enable, std::nullopt, 0x9), set(k_dcb_accumulate, k_rect_c, 0x5), set(0xe, k_rect_a, 0x7),
                  get(0, 0x3, {0, 0, 64, 64}), set(0xe, k_rect_b, 0xb)}},
                {"flags_set_0xb_pre0",
                 {set(0xb, k_rect_a, 0x9), get(0, 0x3, k_rect_a), set(0xb, k_rect_b, 0xb), get(0, 0x3, {50, 60, 64, 64}),
                  set(0xb, std::nullopt, 0xb), get(0, 0x1, k_empty)}},
                {"flags_set_0x10_pre2",
                 {set(k_dcb_enable, std::nullopt, 0x9), set(k_dcb_accumulate, k_rect_c, 0x5), set(0x10, k_rect_a, 0x7),
                  get(0, 0x3, {0, 0, 64, 64}), get(k_dcb_reset, 0x3, {0, 0, 64, 64}), get(0, 0x1, k_empty)}},
                {"flags_set_0x20_pre0", {set(0x20, k_rect_a, 0x9), set(0x20, k_rect_b, 0x9), get(0, 0x1, k_empty)}},
                {"flags_set_0x80000000_pre1",
                 {set(k_dcb_enable, std::nullopt, 0x9), set(0x80000000u, k_rect_a, 0x5), set(0x80000000u, std::nullopt, 0x5),
                  get(0, 0x1, k_empty)}},
                {"flags_set_0x8000_pre2",
                 {set(k_dcb_enable, std::nullopt, 0x9), set(k_dcb_accumulate, k_rect_c, 0x5), set(k_dcb_windowmgr, k_rect_a, 0x8009),
                  get(0, 0x3, {0, 0, 64, 64}), set(k_dcb_windowmgr, k_rect_b, 0x8009), get(k_dcb_reset, 0x3, {0, 0, 64, 64}),
                  get(0, 0x1, k_empty)}},
                {"flags_set_0x8002_pre0",
                 {set(k_dcb_windowmgr | k_dcb_accumulate, k_rect_a, 0x8009), get(0, 0x1, k_empty),
                  set(k_dcb_windowmgr | k_dcb_accumulate, k_rect_b, 0x800b), set(k_dcb_windowmgr | k_dcb_accumulate, std::nullopt, 0x800b),
                  get(k_dcb_reset, 0x1, k_empty)}},
                {"flags_set_0x8004_pre0",
                 {set(k_dcb_windowmgr | k_dcb_enable, k_rect_a, 0x8009), set(k_dcb_windowmgr | k_dcb_enable, k_rect_b, 0x8005),
                  set(0, std::nullopt, 0x9)}},
                {"flags_set_0x8006_pre0",
                 {set(k_dcb_windowmgr | 0x6, k_rect_a, 0x8009), set(k_dcb_windowmgr | 0x6, k_rect_b, 0x8007), get(0, 0x1, k_empty)}},
                {"windowmgr",
                 {set(k_dcb_enable, std::nullopt, 0x9), set(k_dcb_accumulate, k_rect_a, 0x5), get(0, 0x3, {0, 0, 30, 40}),
                  set(k_dcb_windowmgr | k_dcb_accumulate, k_rect_b, 0x8009), get(0, 0x3, {0, 0, 30, 40}),
                  get(k_dcb_windowmgr, 0x3, {0, 0, 70, 80}), get(k_dcb_windowmgr | k_dcb_reset, 0x3, {0, 0, 70, 80}),
                  get(0, 0x3, {0, 0, 30, 40}), get(k_dcb_windowmgr, 0x1, k_empty),
                  set(k_dcb_windowmgr | k_dcb_enable, std::nullopt, 0x8009), get(k_dcb_windowmgr, 0x1, k_empty)}},
                {"getflags_0x1",
                 {set(k_dcb_enable, std::nullopt, 0x9), set(k_dcb_accumulate, k_rect_a, 0x5), get(0, 0x3, {0, 0, 30, 40}),
                  get(0x1, 0x3, {0, 0, 30, 40}), get(0, 0x1, k_empty), get(0x1, 0x1, k_empty)}},
                {"getflags_0x3_reset", {set(k_dcb_accumulate, k_rect_a, 0x9), get(0x3, 0x3, {0, 0, 30, 40}), get(0, 0x1, k_empty)}},
                {"getflags_0x5_reset", {set(k_dcb_accumulate, k_rect_a, 0x9), get(0x5, 0x3, {0, 0, 30, 40}), get(0, 0x1, k_empty)}},
                {"getflags_0xf_reset", {set(k_dcb_accumulate, k_rect_a, 0x9), get(0xf, 0x3, {0, 0, 30, 40}), get(0, 0x1, k_empty)}},
                {"getflags_without_reset",
                 {set(k_dcb_accumulate, k_rect_a, 0x9), get(0x2, 0x3, {0, 0, 30, 40}), get(0x4, 0x3, {0, 0, 30, 40}),
                  get(0x8, 0x3, {0, 0, 30, 40}), get(0x10, 0x3, {0, 0, 30, 40}), get(0, 0x3, {0, 0, 30, 40})}},
                {"getflags_0x8000_0x8001_0xffffffff",
                 {set(k_dcb_accumulate, k_rect_a, 0x9), get(0x8000, 0x1, k_empty), get(0, 0x3, {0, 0, 30, 40}), get(0x8001, 0x1, k_empty),
                  get(0, 0x3, {0, 0, 30, 40}), get(0xffffffffu, 0x1, k_empty), get(0, 0x3, {0, 0, 30, 40})}},
                {"rect_empty_f2",
                 {set(k_dcb_accumulate, box{5, 5, 5, 5}, 0x9), get(0, 0x3, {0, 0, 5, 5}), set(k_dcb_accumulate, k_rect_a, 0xb),
                  get(0, 0x3, {0, 0, 30, 40}), get(k_dcb_reset, 0x3, {0, 0, 30, 40}), get(0, 0x1, k_empty)}},
                {"rect_zero_h_f2", {set(k_dcb_accumulate, box{5, 5, 9, 5}, 0x9), get(0, 0x3, {0, 0, 9, 5})}},
                {"rect_zero_w_f6",
                 {set(0x6, box{5, 5, 5, 9}, 0x9), get(0, 0x3, {0, 0, 5, 9}), set(k_dcb_accumulate, k_rect_a, 0x7),
                  get(0, 0x3, {0, 0, 30, 40})}},
                {"rect_inverted_f2", {set(k_dcb_accumulate, box{30, 40, 10, 20}, 0x9), get(0, 0x3, {0, 0, 10, 20})}},
                {"rect_inverted_x_f6", {set(0x6, box{30, 20, 10, 40}, 0x9), get(0, 0x3, {0, 0, 10, 40})}},
                {"rect_negative_f2",
                 {set(k_dcb_accumulate, box{-100, -50, -10, -5}, 0x9), get(0, 0x3, k_empty), set(k_dcb_accumulate, k_rect_a, 0xb),
                  get(0, 0x3, {0, 0, 30, 40})}},
                {"rect_huge_pos_f2", {set(k_dcb_accumulate, box{0, 0, INT32_MAX, INT32_MAX}, 0x9), get(0, 0x3, {0, 0, 64, 64})}},
                {"rect_minmax_f6", {set(0x6, box{INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX}, 0x9), get(0, 0x3, {0, 0, 64, 64})}},
                {"rect_offscreen_f2", {set(k_dcb_accumulate, box{1000, 1000, 2000, 2000}, 0x9), get(0, 0x3, {0, 0, 64, 64})}},
                {"rect_straddle0_f2", {set(k_dcb_accumulate, box{-10, -10, 10, 10}, 0x9), get(0, 0x3, {0, 0, 10, 10})}},
                {"rect_empty_f1",
                 {set(k_dcb_reset, box{5, 5, 5, 5}, 0x9), get(0, 0x1, k_empty), set(k_dcb_accumulate, k_rect_a, 0x9), get(0, 0x3, k_rect_a),
                  get(k_dcb_reset, 0x3, k_rect_a), get(0, 0x1, k_empty)}},
                {"rect_inverted_f1",
                 {set(k_dcb_reset, box{30, 40, 10, 20}, 0x9), set(k_dcb_accumulate, k_rect_a, 0x9), get(0, 0x3, k_rect_a)}},
                {"rect_huge_pos_f4",
                 {set(k_dcb_enable, box{0, 0, INT32_MAX, INT32_MAX}, 0x9), get(0, 0x1, k_empty), set(k_dcb_accumulate, k_rect_a, 0x5),
                  get(0, 0x3, {0, 0, 30, 40})}},
                {"coord_ident_then_aniso_identity_part",
                 {set(k_dcb_enable, std::nullopt, 0x9), set(k_dcb_accumulate, box{20, 20, 40, 40}, 0x5), get(0, 0x3, {0, 0, 40, 40})}},
                {"degenerate_rect_after_reset",
                 {set(k_dcb_reset | k_dcb_enable, std::nullopt, 0x9), set(k_dcb_accumulate, box{10, 10, 10, 20}, 0x5), get(0, 0x1, k_empty),
                  set(0, std::nullopt, 0x5), set(k_dcb_accumulate, box{20, 20, 30, 30}, 0x5), get(0, 0x3, {10, 10, 30, 30})}},
                {"misc_4_comctl32_enable",
                 {set(k_dcb_enable, std::nullopt, 0x9), get(0, 0x1, k_empty), get(k_dcb_reset, 0x1, k_empty), get(0, 0x1, k_empty)}},
                {"misc_5_comctl32_resetenable",
                 {set(k_dcb_reset | k_dcb_enable, std::nullopt, 0x9), get(0, 0x1, k_empty), get(k_dcb_reset, 0x1, k_empty),
                  get(0, 0x1, k_empty)}},
                {"misc_6_comctl32_accumulateenable",
                 {set(k_dcb_accumulate | k_dcb_enable, std::nullopt, 0x9), get(0, 0x1, k_empty), get(k_dcb_reset, 0x1, k_empty),
                  get(0, 0x1, k_empty)}},
                {"misc_8_accumulate_while_disabled",
                 {set(k_dcb_enable, std::nullopt, 0x9), set(k_dcb_accumulate, k_rect_a, 0x5), set(k_dcb_disable, std::nullopt, 0x7),
                  get(0, 0x3, {0, 0, 30, 40}), set(k_dcb_accumulate, k_rect_b, 0xb), get(0, 0x3, {0, 0, 64, 64}),
                  set(0xc, std::nullopt, 0xb), get(0, 0x3, {0, 0, 64, 64})}},
                {"getreset_accumA_getreset_draw_accumB",
                 {set(k_dcb_enable, std::nullopt, 0x9), get(0, 0x1, k_empty), set(k_dcb_accumulate, k_rect_a, 0x5),
                  get(0, 0x3, {0, 0, 30, 40}), get(k_dcb_reset, 0x3, {0, 0, 30, 40}), get(0, 0x1, k_empty),
                  draw_rectangle(box{20, 22, 26, 28}), get(0, 0x3, {20, 22, 26, 28}), set(k_dcb_accumulate, box{35, 36, 45, 46}, 0x7),
                  get(0, 0x3, {20, 22, 45, 46})}},
                {"getreset_accumA_getreset_accumB_draw",
                 {set(k_dcb_enable, std::nullopt, 0x9), get(0, 0x1, k_empty), set(k_dcb_accumulate, k_rect_a, 0x5),
                  get(0, 0x3, {0, 0, 30, 40}), get(k_dcb_reset, 0x3, {0, 0, 30, 40}), get(0, 0x1, k_empty),
                  set(k_dcb_accumulate, box{35, 36, 45, 46}, 0x5), get(0, 0x3, {35, 36, 45, 46}), draw_rectangle(box{20, 22, 26, 28}),
                  get(0, 0x3, {20, 22, 45, 46})}},
                {"getreset_empty_getreset_accumA",
                 {set(k_dcb_enable, std::nullopt, 0x9), get(0, 0x1, k_empty), get(k_dcb_reset, 0x1, k_empty), get(0, 0x1, k_empty),
                  set(k_dcb_accumulate, k_rect_a, 0x5), get(0, 0x3, {0, 0, 30, 40})}},
                {"getreset_baseline_setreset_accumA",
                 {set(k_dcb_enable, std::nullopt, 0x9), get(0, 0x1, k_empty), set(k_dcb_reset, std::nullopt, 0x5), get(0, 0x1, k_empty),
                  set(k_dcb_accumulate, k_rect_a, 0x5), get(0, 0x3, k_rect_a)}},
                {"getreset_draw_getreset_draw",
                 {set(k_dcb_enable, std::nullopt, 0x9), get(0, 0x1, k_empty), draw_rectangle(box{20, 22, 26, 28}),
                  get(0, 0x3, {0, 0, 26, 28}), get(k_dcb_reset, 0x3, {0, 0, 26, 28}), get(0, 0x1, k_empty),
                  draw_rectangle(box{40, 44, 50, 54}), get(0, 0x3, {40, 44, 50, 54})}},
                {"getreset_wm_getreset_accumB",
                 {set(k_dcb_enable | k_dcb_windowmgr, std::nullopt, 0x8009), get(k_dcb_windowmgr, 0x1, k_empty),
                  set(k_dcb_accumulate | k_dcb_windowmgr, k_rect_a, 0x8005), get(k_dcb_windowmgr, 0x3, {0, 0, 30, 40}),
                  get(k_dcb_reset | k_dcb_windowmgr, 0x3, {0, 0, 30, 40}), get(k_dcb_windowmgr, 0x1, k_empty),
                  set(k_dcb_accumulate | k_dcb_windowmgr, box{35, 36, 45, 46}, 0x8005), get(k_dcb_windowmgr, 0x3, {35, 36, 45, 46}),
                  draw_rectangle(box{20, 22, 26, 28}), get(k_dcb_windowmgr, 0x3, {20, 22, 45, 46}), get(0, 0x1, k_empty)}},
                {"getreset_getreset_twice_accumA",
                 {set(k_dcb_enable, std::nullopt, 0x9), get(0, 0x1, k_empty), set(k_dcb_accumulate, k_rect_a, 0x5),
                  get(k_dcb_reset, 0x3, {0, 0, 30, 40}), get(k_dcb_reset, 0x1, k_empty), get(0, 0x1, k_empty),
                  set(k_dcb_accumulate, box{35, 36, 45, 46}, 0x5), get(0, 0x3, {35, 36, 45, 46})}},
            };
        }

        std::vector<state_case> save_restore_cases()
        {
            return {
                {"saverestore_en_dis",
                 {set(k_dcb_accumulate | k_dcb_enable, k_rect_a, 0x9), get(0, 0x3, {0, 0, 30, 40}), save_dc(1),
                  set(k_dcb_disable, std::nullopt, 0x7), get(0, 0x3, {0, 0, 30, 40}), restore_dc(-1), get(0, 0x3, {0, 0, 30, 40}),
                  draw_rectangle(box{40, 44, 50, 54}), get(0, 0x3, {0, 0, 30, 40}), set(k_dcb_accumulate, box{2, 3, 4, 5}, 0xb),
                  get(0, 0x3, {0, 0, 30, 40})}},
                {"saverestore_dis_en",
                 {set(k_dcb_accumulate | k_dcb_enable, k_rect_a, 0x9), set(k_dcb_disable, std::nullopt, 0x7), get(0, 0x3, {0, 0, 30, 40}),
                  save_dc(1), set(k_dcb_accumulate | k_dcb_enable, box{35, 36, 45, 46}, 0xb), get(0, 0x3, {0, 0, 45, 46}), restore_dc(-1),
                  get(0, 0x3, {0, 0, 45, 46}), draw_rectangle(box{40, 44, 50, 54}), get(0, 0x3, {0, 0, 50, 54}),
                  set(k_dcb_accumulate, box{2, 3, 4, 5}, 0x7), get(0, 0x3, {0, 0, 50, 54})}},
                {"saverestore_rect_reset",
                 {set(k_dcb_accumulate | k_dcb_enable, k_rect_a, 0x9), get(0, 0x3, {0, 0, 30, 40}), save_dc(1),
                  set(k_dcb_reset, std::nullopt, 0x7), get(0, 0x1, k_empty), restore_dc(-1), get(0, 0x1, k_empty),
                  draw_rectangle(box{40, 44, 50, 54}), get(0, 0x3, {40, 44, 50, 54}), set(k_dcb_accumulate, box{2, 3, 4, 5}, 0x7),
                  get(0, 0x3, {2, 3, 50, 54})}},
                {"saverestore_rect_other",
                 {set(k_dcb_accumulate | k_dcb_enable, k_rect_a, 0x9), get(0, 0x3, {0, 0, 30, 40}), save_dc(1),
                  set(k_dcb_reset, box{35, 36, 45, 46}, 0x7), set(k_dcb_accumulate, box{35, 36, 45, 46}, 0x5),
                  get(0, 0x3, {35, 36, 45, 46}), restore_dc(-1), get(0, 0x3, {35, 36, 45, 46}), draw_rectangle(box{40, 44, 50, 54}),
                  get(0, 0x3, {35, 36, 50, 54}), set(k_dcb_accumulate, box{2, 3, 4, 5}, 0x7), get(0, 0x3, {2, 3, 50, 54})}},
                {"saverestore_never_en",
                 {get(0, 0x1, k_empty), save_dc(1), set(k_dcb_accumulate | k_dcb_enable, box{35, 36, 45, 46}, 0x9),
                  get(0, 0x3, {0, 0, 45, 46}), restore_dc(-1), get(0, 0x3, {0, 0, 45, 46}), draw_rectangle(box{40, 44, 50, 54}),
                  get(0, 0x3, {0, 0, 50, 54}), set(k_dcb_accumulate, box{2, 3, 4, 5}, 0x7), get(0, 0x3, {0, 0, 50, 54})}},
                {"saverestore_explicit_index",
                 {set(k_dcb_accumulate | k_dcb_enable, k_rect_a, 0x9), get(0, 0x3, {0, 0, 30, 40}), save_dc(1),
                  set(k_dcb_disable, std::nullopt, 0x7), save_dc(2), set(k_dcb_reset | k_dcb_enable, std::nullopt, 0xb),
                  set(k_dcb_accumulate, box{35, 36, 45, 46}, 0x5), get(0, 0x3, {35, 36, 45, 46}), restore_dc(1),
                  get(0, 0x3, {35, 36, 45, 46}), draw_rectangle(box{40, 44, 50, 54}), get(0, 0x3, {35, 36, 50, 54}),
                  set(k_dcb_accumulate, box{2, 3, 4, 5}, 0x7), get(0, 0x3, {2, 3, 50, 54})}},
                {"saverestore_nested_m1",
                 {set(k_dcb_accumulate | k_dcb_enable, k_rect_a, 0x9), get(0, 0x3, {0, 0, 30, 40}), save_dc(1),
                  set(k_dcb_disable, std::nullopt, 0x7), save_dc(2), set(k_dcb_reset | k_dcb_enable, std::nullopt, 0xb),
                  get(0, 0x1, k_empty), restore_dc(-1), get(0, 0x1, k_empty), restore_dc(-1), get(0, 0x1, k_empty),
                  draw_rectangle(box{40, 44, 50, 54}), get(0, 0x3, {40, 44, 50, 54}), set(k_dcb_accumulate, box{2, 3, 4, 5}, 0x7),
                  get(0, 0x3, {2, 3, 50, 54})}},
                {"saverestore_wm_rect_reset",
                 {set(k_dcb_accumulate | k_dcb_enable | k_dcb_windowmgr, k_rect_a, 0x8009), get(k_dcb_windowmgr, 0x3, {0, 0, 30, 40}),
                  get(0, 0x1, k_empty), save_dc(1), set(k_dcb_reset | k_dcb_windowmgr, std::nullopt, 0x8007),
                  get(k_dcb_windowmgr, 0x1, k_empty), get(0, 0x1, k_empty), restore_dc(-1), get(k_dcb_windowmgr, 0x1, k_empty),
                  draw_rectangle(box{40, 44, 50, 54}), get(k_dcb_windowmgr, 0x3, {40, 44, 50, 54}),
                  set(k_dcb_accumulate | k_dcb_windowmgr, box{2, 3, 4, 5}, 0x8007), get(k_dcb_windowmgr, 0x3, {2, 3, 50, 54}),
                  get(0, 0x1, k_empty)}},
                {"saverestore_wm_en_dis",
                 {set(k_dcb_accumulate | k_dcb_enable | k_dcb_windowmgr, k_rect_a, 0x8009), get(k_dcb_windowmgr, 0x3, {0, 0, 30, 40}),
                  get(0, 0x1, k_empty), save_dc(1), set(k_dcb_disable | k_dcb_windowmgr, std::nullopt, 0x8007),
                  get(k_dcb_windowmgr, 0x3, {0, 0, 30, 40}), get(0, 0x1, k_empty), restore_dc(-1),
                  get(k_dcb_windowmgr, 0x3, {0, 0, 30, 40}), draw_rectangle(box{40, 44, 50, 54}), get(k_dcb_windowmgr, 0x3, {0, 0, 30, 40}),
                  set(k_dcb_accumulate | k_dcb_windowmgr, box{2, 3, 4, 5}, 0x800b), get(k_dcb_windowmgr, 0x3, {0, 0, 30, 40}),
                  get(0, 0x1, k_empty)}},
                {"saverestore_wm_rect_other",
                 {set(k_dcb_accumulate | k_dcb_enable | k_dcb_windowmgr, k_rect_a, 0x8009), get(k_dcb_windowmgr, 0x3, {0, 0, 30, 40}),
                  get(0, 0x1, k_empty), save_dc(1), set(k_dcb_reset | k_dcb_windowmgr, box{35, 36, 45, 46}, 0x8007),
                  set(k_dcb_accumulate | k_dcb_windowmgr, box{35, 36, 45, 46}, 0x8005), get(k_dcb_windowmgr, 0x3, {35, 36, 45, 46}),
                  get(0, 0x1, k_empty), restore_dc(-1), get(k_dcb_windowmgr, 0x3, {35, 36, 45, 46}), draw_rectangle(box{40, 44, 50, 54}),
                  get(k_dcb_windowmgr, 0x3, {35, 36, 50, 54}), set(k_dcb_accumulate | k_dcb_windowmgr, box{2, 3, 4, 5}, 0x8007),
                  get(k_dcb_windowmgr, 0x3, {2, 3, 50, 54}), get(0, 0x1, k_empty)}},
            };
        }

        struct outside_case
        {
            std::string_view name{};
            bool reset_first{};
            bool prefilled{};
            box area{};
            box expected{};
            std::optional<box> expected_after_inside_accumulate{};
        };

        std::vector<outside_case> outside_cases()
        {
            return {
                {.name = "enabled_far_pos",
                 .reset_first = false,
                 .prefilled = false,
                 .area = {1000, 1000, 1064, 1064},
                 .expected = {0, 0, 64, 64},
                 .expected_after_inside_accumulate = box{0, 0, 64, 64}},
                {.name = "enabled_far_neg",
                 .reset_first = false,
                 .prefilled = false,
                 .area = {-100, -100, -50, -50},
                 .expected = {0, 0, 0, 0},
                 .expected_after_inside_accumulate = box{0, 0, 45, 46}},
                {.name = "enabled_straddle_neg",
                 .reset_first = false,
                 .prefilled = false,
                 .area = {-10, -10, 10, 10},
                 .expected = {0, 0, 10, 10},
                 .expected_after_inside_accumulate = box{0, 0, 45, 46}},
                {.name = "enabled_straddle_pos",
                 .reset_first = false,
                 .prefilled = false,
                 .area = {60, 60, 100, 100},
                 .expected = {0, 0, 64, 64},
                 .expected_after_inside_accumulate = box{0, 0, 64, 64}},
                {.name = "enabled_right_only",
                 .reset_first = false,
                 .prefilled = false,
                 .area = {100, 10, 150, 20},
                 .expected = {0, 0, 64, 20},
                 .expected_after_inside_accumulate = box{0, 0, 64, 46}},
                {.name = "enabled_below_only",
                 .reset_first = false,
                 .prefilled = false,
                 .area = {10, 100, 20, 150},
                 .expected = {0, 0, 20, 64},
                 .expected_after_inside_accumulate = box{0, 0, 45, 64}},
                {.name = "resetenabled_far_pos",
                 .reset_first = true,
                 .prefilled = false,
                 .area = {1000, 1000, 1064, 1064},
                 .expected = {1000, 1000, 64, 64},
                 .expected_after_inside_accumulate = box{35, 36, 64, 64}},
                {.name = "resetenabled_far_neg",
                 .reset_first = true,
                 .prefilled = false,
                 .area = {-100, -100, -50, -50},
                 .expected = {0, 0, -50, -50},
                 .expected_after_inside_accumulate = box{0, 0, 45, 46}},
                {.name = "resetenabled_straddle_neg",
                 .reset_first = true,
                 .prefilled = false,
                 .area = {-10, -10, 10, 10},
                 .expected = {0, 0, 10, 10},
                 .expected_after_inside_accumulate = box{0, 0, 45, 46}},
                {.name = "resetenabled_straddle_pos",
                 .reset_first = true,
                 .prefilled = false,
                 .area = {60, 60, 100, 100},
                 .expected = {60, 60, 64, 64},
                 .expected_after_inside_accumulate = box{35, 36, 64, 64}},
                {.name = "resetenabled_right_only",
                 .reset_first = true,
                 .prefilled = false,
                 .area = {100, 10, 150, 20},
                 .expected = {100, 10, 64, 20},
                 .expected_after_inside_accumulate = box{35, 10, 64, 46}},
                {.name = "resetenabled_below_only",
                 .reset_first = true,
                 .prefilled = false,
                 .area = {10, 100, 20, 150},
                 .expected = {10, 100, 20, 64},
                 .expected_after_inside_accumulate = box{10, 36, 45, 64}},
                {.name = "nonempty_far_pos",
                 .reset_first = false,
                 .prefilled = true,
                 .area = {1000, 1000, 1064, 1064},
                 .expected = {0, 0, 64, 64},
                 .expected_after_inside_accumulate = std::nullopt},
                {.name = "nonempty_far_neg",
                 .reset_first = false,
                 .prefilled = true,
                 .area = {-100, -100, -50, -50},
                 .expected = {0, 0, 30, 40},
                 .expected_after_inside_accumulate = std::nullopt},
                {.name = "nonempty_straddle_neg",
                 .reset_first = false,
                 .prefilled = true,
                 .area = {-10, -10, 10, 10},
                 .expected = {0, 0, 30, 40},
                 .expected_after_inside_accumulate = std::nullopt},
                {.name = "nonempty_straddle_pos",
                 .reset_first = false,
                 .prefilled = true,
                 .area = {60, 60, 100, 100},
                 .expected = {0, 0, 64, 64},
                 .expected_after_inside_accumulate = std::nullopt},
                {.name = "nonempty_right_only",
                 .reset_first = false,
                 .prefilled = true,
                 .area = {100, 10, 150, 20},
                 .expected = {0, 0, 64, 40},
                 .expected_after_inside_accumulate = std::nullopt},
                {.name = "nonempty_below_only",
                 .reset_first = false,
                 .prefilled = true,
                 .area = {10, 100, 20, 150},
                 .expected = {0, 0, 30, 64},
                 .expected_after_inside_accumulate = std::nullopt},
            };
        }
    }

    TEST_F(GdiBoundsRectTest, FollowsTheWindowsStateMachineAndRejectsBadArguments)
    {
        for (const auto& test_case : flag_and_rectangle_cases())
        {
            this->run(this->create_dc(), test_case);
        }

        this->expect_invalid_handles_fail();
        this->expect_unusable_rect_pointers_fail();
    }

    TEST_F(GdiBoundsRectTest, KeepsTheBoundsPerDcAcrossSaveRestoreAndFollowsTheSelectedBitmap)
    {
        for (const auto& test_case : save_restore_cases())
        {
            this->run(this->create_dc(), test_case);
        }

        const auto dc = this->create_dc();
        EXPECT_EQ(0x9u, this->set_bounds(dc, std::nullopt, k_dcb_enable));
        EXPECT_EQ(0x5u, this->set_bounds(dc, k_rect_a, k_dcb_accumulate));
        EXPECT_EQ(1, syscalls::handle_NtGdiSaveDC(this->context(), dc));
        EXPECT_EQ(0x7u, this->set_bounds(dc, std::nullopt, k_dcb_reset | k_dcb_disable));
        this->expect_bounds(dc, 0x1, k_empty);
        EXPECT_EQ(TRUE, syscalls::handle_NtGdiRestoreDC(this->context(), dc, -1));
        this->expect_bounds(dc, 0x1, k_empty);

        const auto resized = this->create_dc();
        EXPECT_EQ(0x9u, this->set_bounds(resized, std::nullopt, k_dcb_enable));
        EXPECT_EQ(0x5u, this->set_bounds(resized, k_rect_a, k_dcb_accumulate));
        const auto smaller = syscalls::handle_NtGdiCreateBitmap(this->context(), 32, 32, 1, 32, 0);
        syscalls::handle_NtGdiSelectBitmap(this->context(), resized, make_handle(smaller));
        this->expect_bounds(resized, 0x3, {0, 0, 30, 32});

        const auto larger = syscalls::handle_NtGdiCreateBitmap(this->context(), 128, 128, 1, 32, 0);
        syscalls::handle_NtGdiSelectBitmap(this->context(), resized, make_handle(larger));
        this->expect_bounds(resized, 0x3, {0, 0, 30, 40});

        const auto kept = this->create_dc();
        EXPECT_EQ(0x9u, this->set_bounds(kept, std::nullopt, k_dcb_reset | k_dcb_enable));
        EXPECT_EQ(0x5u, this->set_bounds(kept, box{10, 10, 100, 100}, k_dcb_accumulate));
        this->expect_bounds(kept, 0x3, {10, 10, 64, 64});
        const auto bigger = syscalls::handle_NtGdiCreateBitmap(this->context(), 128, 128, 1, 32, 0);
        const auto original = syscalls::handle_NtGdiSelectBitmap(this->context(), kept, make_handle(bigger));
        this->expect_bounds(kept, 0x3, {10, 10, 100, 100});
        EXPECT_EQ(1, syscalls::handle_NtGdiSaveDC(this->context(), kept));
        syscalls::handle_NtGdiSelectBitmap(this->context(), kept, make_handle(original));
        this->expect_bounds(kept, 0x3, {10, 10, 64, 64});
        EXPECT_EQ(TRUE, syscalls::handle_NtGdiRestoreDC(this->context(), kept, -1));
        this->expect_bounds(kept, 0x3, {10, 10, 100, 100});

        const auto compatible = static_cast<hdc>(syscalls::handle_NtGdiCreateCompatibleDC(this->context(), resized));
        this->expect_bounds(compatible, 0x1, k_empty);

        const auto fresh = static_cast<hdc>(syscalls::handle_NtGdiCreateCompatibleDC(this->context(), 0));
        EXPECT_EQ(0x9u, this->set_bounds(fresh, box{1, 2, 3, 4}, k_dcb_enable));
        EXPECT_EQ(0x5u, this->set_bounds(fresh, box{1, 2, 3, 4}, k_dcb_accumulate));
        EXPECT_EQ(0x7u, this->set_bounds(fresh, std::nullopt, 0));
        this->expect_bounds(fresh, 0x3, {0, 0, 1, 1});
    }

    TEST_F(GdiBoundsRectTest, AccumulatesTheExtentOfDrawingIncludingTheQueuedBatch)
    {
        struct primitive_case
        {
            std::string_view name{};
            primitive kind{};
            box first_extent{};
            box both_extents{};
        };

        const std::array<primitive_case, 6> primitives{{
            {.name = "Rectangle", .kind = primitive::rectangle, .first_extent = {10, 12, 30, 28}, .both_extents = {10, 12, 58, 52}},
            {.name = "PatBlt", .kind = primitive::pat_blt, .first_extent = {10, 12, 30, 28}, .both_extents = {10, 12, 58, 52}},
            {.name = "BitBlt", .kind = primitive::bit_blt, .first_extent = {10, 12, 30, 28}, .both_extents = {10, 12, 58, 52}},
            {.name = "StretchBlt", .kind = primitive::stretch_blt, .first_extent = {10, 12, 30, 28}, .both_extents = {10, 12, 58, 52}},
            {.name = "LineTo", .kind = primitive::line_to, .first_extent = {10, 12, 31, 29}, .both_extents = {10, 12, 59, 53}},
            {.name = "SetPixel", .kind = primitive::set_pixel, .first_extent = {10, 12, 11, 13}, .both_extents = {10, 12, 41, 37}},
        }};

        constexpr box first_area{10, 12, 30, 28};
        constexpr box second_area{40, 36, 58, 52};
        constexpr box disabled_area{2, 54, 10, 62};

        for (const auto& drawn : primitives)
        {
            for (const auto start_flags : {k_dcb_reset | k_dcb_enable, k_dcb_enable})
            {
                SCOPED_TRACE(std::string(drawn.name) + (start_flags == k_dcb_enable ? " enable" : " resetenable"));
                const auto dc = this->create_dc();
                const auto source = this->create_dc(32);
                if (start_flags == k_dcb_enable)
                {
                    EXPECT_EQ(0x9u, this->set_bounds(dc, std::nullopt, k_dcb_reset));
                }
                EXPECT_EQ(0x9u, this->set_bounds(dc, std::nullopt, start_flags));
                this->expect_bounds(dc, 0x1, k_empty);

                this->draw(drawn.kind, dc, source, first_area);
                this->expect_bounds(dc, 0x3, drawn.first_extent);
                this->draw(drawn.kind, dc, source, second_area);
                this->expect_bounds(dc, 0x3, drawn.both_extents);

                EXPECT_EQ(0x7u, this->set_bounds(dc, std::nullopt, k_dcb_disable));
                this->draw(drawn.kind, dc, source, disabled_area);
                this->expect_bounds(dc, 0x3, drawn.both_extents);

                box rect{};
                EXPECT_EQ(0x3u, this->get_bounds(dc, k_dcb_reset, rect));
                EXPECT_EQ(drawn.both_extents, rect);
                this->expect_bounds(dc, 0x1, k_empty);
            }
        }

        for (const auto& test_case : outside_cases())
        {
            SCOPED_TRACE(test_case.name);
            for (const bool through_drawing : {false, true})
            {
                SCOPED_TRACE(through_drawing ? "drawing" : "set");
                const auto dc = this->create_dc();
                EXPECT_EQ(0x9u, this->set_bounds(dc, std::nullopt, test_case.reset_first ? k_dcb_reset | k_dcb_enable : k_dcb_enable));
                if (test_case.prefilled)
                {
                    EXPECT_EQ(0x5u, this->set_bounds(dc, k_rect_a, k_dcb_accumulate));
                }

                if (through_drawing)
                {
                    this->draw(primitive::rectangle, dc, dc, test_case.area);
                }
                else
                {
                    this->set_bounds(dc, test_case.area, k_dcb_accumulate);
                }
                this->expect_bounds(dc, 0x3, test_case.expected);

                if (!through_drawing && test_case.expected_after_inside_accumulate)
                {
                    EXPECT_EQ(0x7u, this->set_bounds(dc, k_rect_inside, k_dcb_accumulate));
                    this->expect_bounds(dc, 0x3, *test_case.expected_after_inside_accumulate);
                }
            }
        }

        for (const box& empty_area : {box{20, 20, 20, 25}, box{20, 20, 25, 20}})
        {
            const auto dc = this->create_dc();
            EXPECT_EQ(0x9u, this->set_bounds(dc, std::nullopt, k_dcb_reset | k_dcb_enable));
            EXPECT_EQ(0x5u, this->set_bounds(dc, box{1, 1, 2, 2}, k_dcb_accumulate));
            this->draw(primitive::pat_blt, dc, dc, empty_area);
            this->expect_bounds(dc, 0x3, {1, 1, 2, 2});
        }

        const auto batched = this->create_dc();
        EXPECT_EQ(0x9u, this->set_bounds(batched, std::nullopt, k_dcb_reset | k_dcb_enable));
        this->queue_opaque_rectangle(batched, {10, 12, 30, 28});
        this->expect_bounds(batched, 0x3, {10, 12, 30, 28});

        const auto reset_after_batch = this->create_dc();
        EXPECT_EQ(0x9u, this->set_bounds(reset_after_batch, std::nullopt, k_dcb_reset | k_dcb_enable));
        this->queue_opaque_rectangle(reset_after_batch, {10, 12, 30, 28});
        EXPECT_EQ(0x7u, this->set_bounds(reset_after_batch, std::nullopt, k_dcb_reset));
        this->expect_bounds(reset_after_batch, 0x1, k_empty);
    }
}
