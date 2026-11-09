#include "emulation_test_utils.hpp"
#include <registry/registry_file.hpp>
#include <registry/registry_state_file.hpp>
#include <utils/io.hpp>
#include <utils/finally.hpp>

#include <atomic>
#include <fstream>

#ifndef OS_WINDOWS
#include <csignal>
#include <sys/resource.h>
#endif

namespace sogen::test
{
#ifndef OS_EMSCRIPTEN
    namespace
    {
        class RegistryStateFileTest : public testing::Test
        {
          protected:
            void SetUp() override
            {
                static std::atomic<uint64_t> next{};
                this->path = std::filesystem::temp_directory_path() /
                             ("sogen-registry-state-" + std::to_string(getpid()) + "-" + std::to_string(next.fetch_add(1)));
            }

            void TearDown() override
            {
                std::error_code ignored{};
                std::filesystem::remove(this->path, ignored);
                auto lock = this->path;
                lock += ".lock";
                std::filesystem::remove(lock, ignored);
            }

            registry_manager registry()
            {
                return registry_manager{get_emulator_root() / "registry"};
            }

            registry_key key(registry_manager& registry)
            {
                auto value = registry.create_key(std::filesystem::path{u"\\Registry\\Machine\\Software\\OverlayPersistenceTest"});
                if (!value)
                {
                    throw std::runtime_error("Cannot create the synthetic registry key");
                }
                return std::move(*value);
            }

            void set(registry_manager& registry, const registry_key& key, const uint32_t value)
            {
                registry.set_value(key, "Number", REG_DWORD, std::as_bytes(std::span{&value, 1}));
            }

            void expect_number(registry_manager& registry, const uint32_t value)
            {
                const auto key =
                    registry.get_key(utils::path_key{std::filesystem::path{u"\\Registry\\Machine\\Software\\OverlayPersistenceTest"}});
                ASSERT_TRUE(key);
                const auto number = registry.get_value(*key, "Number");
                ASSERT_TRUE(number);
                EXPECT_EQ(value, number->as_dword());
            }

            std::filesystem::path path{};
        };
    }

    TEST_F(RegistryStateFileTest, ValuesAndDeletionsPersistAcrossIndependentRuns)
    {
        {
            auto first = this->registry();
            registry_state_file file{this->path};
            file.load(first);
            const auto key = this->key(first);
            this->set(first, key, 42);
            const std::array<std::byte, 3> bytes{std::byte{1}, std::byte{2}, std::byte{3}};
            first.set_value(key, "Binary", REG_BINARY, bytes);
            file.save(first);
        }
        {
            auto second = this->registry();
            registry_state_file file{this->path};
            file.load(second);
            this->expect_number(second, 42);
            const auto key = this->key(second);
            const auto binary = second.get_value(key, "Binary");
            ASSERT_TRUE(binary);
            EXPECT_EQ(REG_BINARY, binary->type);
            EXPECT_EQ(3, binary->data.size());
            EXPECT_EQ(std::byte{3}, binary->data[2]);
            EXPECT_TRUE(second.delete_value(key, "Number"));
            constexpr std::u16string_view changed = u"changed";
            second.set_value(key, "Binary", REG_SZ, std::as_bytes(std::span{changed}));
            file.save(second);
        }
        {
            auto third = this->registry();
            registry_state_file file{this->path};
            file.load(third);
            const auto key = this->key(third);
            EXPECT_FALSE(third.get_value(key, "Number"));
            const auto changed = third.get_value(key, "Binary");
            ASSERT_TRUE(changed);
            EXPECT_EQ(REG_SZ, changed->type);
            EXPECT_EQ(u"changed", changed->as_string());
            third.delete_key(key);
            file.save(third);
        }
        {
            auto fourth = this->registry();
            registry_state_file file{this->path};
            file.load(fourth);
            EXPECT_FALSE(fourth.get_key(utils::path_key{std::filesystem::path{u"\\Registry\\Machine\\Software\\OverlayPersistenceTest"}}));
            const auto key = this->key(fourth);
            EXPECT_FALSE(fourth.get_value(key, "Number"));
            EXPECT_FALSE(fourth.get_value(key, "Binary"));
        }
    }

    TEST_F(RegistryStateFileTest, ExplicitImportsOverrideLoadedValuesAndReplacementPersists)
    {
        {
            auto first = this->registry();
            registry_state_file file{this->path};
            this->set(first, this->key(first), 1);
            file.save(first);
        }
        {
            auto second = this->registry();
            registry_state_file file{this->path};
            file.load(second);
            constexpr std::string_view contents = "Windows Registry Editor Version 5.00\n\n"
                                                  "[HKEY_LOCAL_MACHINE\\Software\\OverlayPersistenceTest]\n"
                                                  "\"Number\"=dword:00000002\n";
            import_registry_file_contents(second, std::as_bytes(std::span{contents}));
            this->expect_number(second, 2);
            file.save(second);
        }
        auto third = this->registry();
        registry_state_file file{this->path};
        file.load(third);
        this->expect_number(third, 2);
    }

    TEST_F(RegistryStateFileTest, MissingStateDoesNotReplaceTheCurrentOverlay)
    {
        auto registry = this->registry();
        this->set(registry, this->key(registry), 7);
        registry_state_file file{this->path};
        file.load(registry);
        this->expect_number(registry, 7);
        EXPECT_FALSE(std::filesystem::exists(this->path));
    }

    TEST_F(RegistryStateFileTest, MalformedFilesDoNotChangeTheLiveOverlay)
    {
        auto registry = this->registry();
        this->set(registry, this->key(registry), 7);
        registry_state_file file{this->path};
        file.save(registry);
        const auto valid = utils::io::read_file(this->path);
        this->set(registry, this->key(registry), 99);
        std::vector<std::vector<std::byte>> bad;
        bad.emplace_back();
        bad.emplace_back(valid.begin(), valid.begin() + 10);
        bad.push_back(valid);
        bad.back()[8] = std::byte{2};
        bad.push_back(valid);
        bad.back().pop_back();
        bad.push_back(valid);
        bad.back().push_back(std::byte{});
        const auto length = static_cast<uint64_t>(bad.back().size() - 24);
        std::memcpy(bad.back().data() + 16, &length, sizeof(length));
        bad.push_back(valid);
        std::fill_n(bad.back().begin() + 25, 8, std::byte{0xFF});
        for (const auto& bytes : bad)
        {
            ASSERT_TRUE(utils::io::write_file(this->path, bytes));
            EXPECT_THROW(file.load(registry), std::runtime_error);
            this->expect_number(registry, 99);
        }
        ASSERT_TRUE(utils::io::write_file(this->path, valid));
        file.load(registry);
        this->expect_number(registry, 7);
    }

    TEST_F(RegistryStateFileTest, ASecondWriterIsRejectedAndTheLockIsReleasedOnDestruction)
    {
        {
            registry_state_file first{this->path};
            const auto alias = this->path.parent_path() / "." / this->path.filename();
            EXPECT_THROW(registry_state_file second{alias}, std::system_error);
        }
        EXPECT_NO_THROW(registry_state_file next{this->path});
    }

    TEST_F(RegistryStateFileTest, OversizedStateIsRejectedWithoutReplacingTheFile)
    {
        auto registry = this->registry();
        const auto key = this->key(registry);
        this->set(registry, key, 7);
        registry_state_file file{this->path};
        file.save(registry);
        const auto original = utils::io::read_file(this->path);
        const std::vector<std::byte> large(16 * 1024 * 1024, std::byte{0x42});
        registry.set_value(key, "Large", REG_BINARY, large);
        EXPECT_THROW(file.save(registry), std::runtime_error);
        EXPECT_EQ(original, utils::io::read_file(this->path));
        std::filesystem::resize_file(this->path, 16 * 1024 * 1024 + 25);
        EXPECT_THROW(file.load(registry), std::runtime_error);
        this->expect_number(registry, 7);
    }

#ifndef OS_WINDOWS
    TEST_F(RegistryStateFileTest, APartialWriteReportsFailureAndPreservesThePreviousState)
    {
        auto registry = this->registry();
        const auto key = this->key(registry);
        this->set(registry, key, 7);
        registry_state_file file{this->path};
        file.save(registry);
        const auto original = utils::io::read_file(this->path);
        this->set(registry, key, 99);
        rlimit previous{};
        ASSERT_EQ(0, getrlimit(RLIMIT_FSIZE, &previous));
        const auto handler = std::signal(SIGXFSZ, SIG_IGN);
        ASSERT_NE(SIG_ERR, handler);
        const auto restore = utils::finally([&] {
            setrlimit(RLIMIT_FSIZE, &previous);
            std::signal(SIGXFSZ, handler);
        });
        auto limit = previous;
        limit.rlim_cur = 64;
        ASSERT_EQ(0, setrlimit(RLIMIT_FSIZE, &limit));
        EXPECT_THROW(file.save(registry), std::system_error);
        ASSERT_EQ(0, setrlimit(RLIMIT_FSIZE, &previous));
        EXPECT_EQ(original, utils::io::read_file(this->path));
    }
#endif

    TEST_F(RegistryStateFileTest, TruncatedRuntimeRestoreDoesNotDiscardExistingValues)
    {
        auto registry = this->registry();
        this->set(registry, this->key(registry), 7);
        auto replacement = this->registry();
        this->set(replacement, this->key(replacement), 99);
        utils::buffer_serializer buffer{};
        replacement.serialize_runtime_state(buffer);
        auto data = buffer.move_buffer();
        data.pop_back();
        utils::buffer_deserializer input{data};
        EXPECT_THROW(registry.deserialize_runtime_state(input), std::runtime_error);
        this->expect_number(registry, 7);
    }
#endif
}
