#include "std_include.hpp"

#include <devices/vulkan_object_table.hpp>

#include <thread>
#include <unordered_set>

namespace sogen::test
{
    TEST(VulkanObjectTableTest, OnlyTheRecordingThreadNotesItsLookups)
    {
#if defined(OS_EMSCRIPTEN) && !defined(__EMSCRIPTEN_PTHREADS__)
        GTEST_SKIP() << "Requires host threads";
#endif
        vulkan_object_table<int> objects{};
        objects.emplace(1, 0);
        objects.emplace(2, 0);

        std::unordered_set<uint64_t> references{};
        vulkan_recording_references = &references;
        bool found_elsewhere = false;
        std::thread([&] { found_elsewhere = objects.find(2) != objects.end(); }).join();
        const bool found_here = objects.find(1) != objects.end();
        vulkan_recording_references = nullptr;

        EXPECT_TRUE(found_elsewhere);
        EXPECT_TRUE(found_here);
        EXPECT_EQ(references, (std::unordered_set<uint64_t>{1}));
    }
}
