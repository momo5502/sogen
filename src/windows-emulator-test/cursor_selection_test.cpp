#include "emulation_test_utils.hpp"
#include <syscall_utils.hpp>

namespace sogen::syscalls
{
    hcursor handle_NtUserSetCursor(const syscall_context& c, hcursor cursor);
    int32_t handle_NtUserShowCursor(const syscall_context& c, BOOL show);
    hicon handle_NtUserFindExistingCursorIcon(const syscall_context& c, emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> module_name,
                                              emulator_object<UNICODE_STRING<EmulatorTraits<Emu64>>> resource,
                                              emulator_object<EMU_CURSORFIND> find);
} // namespace sogen::syscalls

namespace sogen::test
{
    namespace
    {
        class cursor_ui : public ui_backend
        {
          public:
            void set_event_sink(event_sink) override
            {
            }

            void pump_events() override
            {
            }

            void set_system_cursor(const uint32_t idc) override
            {
                this->shape = idc;
            }

            void set_cursor_visibility(const bool value) override
            {
                this->visible = value;
            }

            std::optional<uint32_t> shape{};
            bool visible{};
        };

        class CursorSelectionTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                auto backend = std::make_unique<cursor_ui>();
                this->ui = backend.get();
                emulator_interfaces interfaces{};
                interfaces.ui = std::move(backend);
                this->emu =
                    std::make_unique<windows_emulator>(create_x86_64_emulator_from_environment(), get_sample_app_settings({}),
                                                       emulator_settings{.disable_logging = true, .emulation_root = get_emulator_root()},
                                                       emulator_callbacks{}, std::move(interfaces));
                this->emu->setup_process_if_necessary();
                this->storage = this->emu->memory.allocate_memory(0x10000, memory_permission::read_write);
            }

            syscall_context context()
            {
                return {.win_emu = *this->emu, .emu = this->emu->vcpu(0).cpu, .vcpu = this->emu->vcpu(0), .proc = this->emu->process};
            }

            hcursor stock(const uint16_t id)
            {
                const auto module_address = this->storage + 0x9000;
                const auto resource_address = module_address + 0x100;
                const auto find_address = resource_address + 0x100;
                const auto text_address = find_address + 0x100;
                constexpr std::u16string_view module = u"USER32";
                this->emu->memory.write_memory(text_address, module.data(), module.size() * sizeof(char16_t));
                const UNICODE_STRING<EmulatorTraits<Emu64>> name{
                    .Length = static_cast<uint16_t>(module.size() * sizeof(char16_t)),
                    .MaximumLength = static_cast<uint16_t>(module.size() * sizeof(char16_t)),
                    .Buffer = text_address,
                };
                const UNICODE_STRING<EmulatorTraits<Emu64>> resource{.Length = 0, .MaximumLength = 0, .Buffer = id};
                const EMU_CURSORFIND find{.hcur = 0, .rt = 1, .cx = 0, .cy = 0, .bpp = 0};
                this->emu->memory.write_memory(module_address, name);
                this->emu->memory.write_memory(resource_address, resource);
                this->emu->memory.write_memory(find_address, find);
                const auto cursor =
                    syscalls::handle_NtUserFindExistingCursorIcon(this->context(), {this->emu->memory, module_address},
                                                                  {this->emu->memory, resource_address}, {this->emu->memory, find_address});
                if (cursor == 0)
                {
                    throw std::runtime_error("Cannot load the stock cursor");
                }
                return cursor;
            }

            void check_restore(const bool snapshot, const bool custom, const bool hidden)
            {
                const auto wait = this->stock(32514);
                const auto text = this->stock(32513);
                auto selected = wait;
                if (custom)
                {
                    auto [handle, cursor] = this->emu->process.cursor_icons.create(this->emu->memory);
                    cursor.type = 1;
                    cursor.module_name = u"sample.dll";
                    cursor.resource_id = 32514;
                    selected = handle.bits;
                }
                syscalls::handle_NtUserSetCursor(this->context(), selected);
                if (hidden)
                {
                    syscalls::handle_NtUserShowCursor(this->context(), FALSE);
                }
                utils::buffer_serializer saved{};
                if (snapshot)
                {
                    this->emu->save_snapshot();
                }
                else
                {
                    this->emu->serialize(saved);
                }
                syscalls::handle_NtUserSetCursor(this->context(), text);
                if (hidden)
                {
                    syscalls::handle_NtUserShowCursor(this->context(), TRUE);
                }
                ASSERT_EQ(32513u, this->ui->shape);
                if (snapshot)
                {
                    this->emu->restore_snapshot();
                }
                else
                {
                    utils::buffer_deserializer reader{saved.get_buffer()};
                    this->emu->deserialize(reader);
                }
                EXPECT_EQ(selected, this->emu->process.current_cursor);
                EXPECT_EQ(custom ? 32512u : 32514u, this->ui->shape);
                EXPECT_EQ(!hidden, this->ui->visible);
            }

            std::unique_ptr<windows_emulator> emu{};
            cursor_ui* ui{};
            uint64_t storage{};
        };
    } // namespace

    TEST_F(CursorSelectionTest, StockSelectionsChangeTheHostShapeAndReturnThePreviousHandle)
    {
        const auto text = this->stock(32513);
        const auto hand = this->stock(32649);
        EXPECT_EQ(hcursor{}, syscalls::handle_NtUserSetCursor(this->context(), text));
        EXPECT_EQ(32513u, this->ui->shape);
        EXPECT_TRUE(this->ui->visible);
        EXPECT_EQ(text, syscalls::handle_NtUserSetCursor(this->context(), hand));
        EXPECT_EQ(32649u, this->ui->shape);
        EXPECT_EQ(hand, this->emu->process.current_cursor);
    }

    TEST_F(CursorSelectionTest, NullAndShowCursorKeepShapeAndVisibilityIndependent)
    {
        const auto text = this->stock(32513);
        const auto arrow = this->stock(32512);
        syscalls::handle_NtUserSetCursor(this->context(), text);
        EXPECT_EQ(-1, syscalls::handle_NtUserShowCursor(this->context(), FALSE));
        EXPECT_FALSE(this->ui->visible);
        EXPECT_EQ(text, syscalls::handle_NtUserSetCursor(this->context(), arrow));
        EXPECT_EQ(32512u, this->ui->shape);
        EXPECT_FALSE(this->ui->visible);
        EXPECT_EQ(0, syscalls::handle_NtUserShowCursor(this->context(), TRUE));
        EXPECT_TRUE(this->ui->visible);
        EXPECT_EQ(arrow, syscalls::handle_NtUserSetCursor(this->context(), 0));
        EXPECT_FALSE(this->ui->visible);
        EXPECT_EQ(32512u, this->ui->shape);
        EXPECT_EQ(hcursor{}, syscalls::handle_NtUserSetCursor(this->context(), text));
        EXPECT_TRUE(this->ui->visible);
        EXPECT_EQ(32513u, this->ui->shape);
    }

    TEST_F(CursorSelectionTest, NonStockCursorResourcesSelectTheNeutralArrow)
    {
        const auto text = this->stock(32513);
        syscalls::handle_NtUserSetCursor(this->context(), text);
        auto [handle, custom] = this->emu->process.cursor_icons.create(this->emu->memory);
        custom.type = 1;
        custom.module_name = u"sample.dll";
        custom.resource_id = 32512;
        EXPECT_EQ(text, syscalls::handle_NtUserSetCursor(this->context(), handle.bits));
        EXPECT_EQ(32512u, this->ui->shape);
        EXPECT_TRUE(this->ui->visible);
        custom.module_name = u"USER32";
        custom.resource_name = u"NamedCursor";
        syscalls::handle_NtUserSetCursor(this->context(), text);
        syscalls::handle_NtUserSetCursor(this->context(), handle.bits);
        EXPECT_EQ(32512u, this->ui->shape);
        custom.resource_name.clear();
        custom.type = 3;
        syscalls::handle_NtUserSetCursor(this->context(), text);
        syscalls::handle_NtUserSetCursor(this->context(), handle.bits);
        EXPECT_EQ(32512u, this->ui->shape);
        custom.type = 1;
        custom.resource_id = 42;
        syscalls::handle_NtUserSetCursor(this->context(), text);
        syscalls::handle_NtUserSetCursor(this->context(), handle.bits);
        EXPECT_EQ(32512u, this->ui->shape);
    }

    TEST_F(CursorSelectionTest, SnapshotRestoresTheStockCursorShapeAndVisibility)
    {
        this->check_restore(true, false, false);
        this->check_restore(true, false, true);
    }

    TEST_F(CursorSelectionTest, DeserializationRestoresTheStockCursorShapeAndVisibility)
    {
        this->check_restore(false, false, false);
        this->check_restore(false, false, true);
    }

    TEST_F(CursorSelectionTest, SnapshotRestoresTheNeutralArrowForCustomCursors)
    {
        this->check_restore(true, true, false);
    }

    TEST_F(CursorSelectionTest, DeserializationRestoresTheNeutralArrowForCustomCursors)
    {
        this->check_restore(false, true, false);
    }
} // namespace sogen::test
