#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "vk_bridge_marshal.generated.hpp"

namespace sogen::test
{
    namespace marshal = gpu_bridge::marshal;

    TEST(VulkanMarshalTest, ApplicationInfoRoundTrip)
    {
        VkApplicationInfo source{};
        source.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        source.pApplicationName = "sogen-app";
        source.applicationVersion = 0x00010203;
        source.pEngineName = "sogen-engine";
        source.engineVersion = 7;
        source.apiVersion = VK_API_VERSION_1_2;

        std::vector<std::byte> buffer;
        marshal::writer writer{buffer};
        marshal::encode(writer, source);

        marshal::reader reader{buffer.data(), buffer.size()};
        marshal::arena arena;
        VkApplicationInfo decoded{};
        ASSERT_TRUE(marshal::decode(reader, arena, decoded));

        EXPECT_EQ(decoded.sType, source.sType);
        EXPECT_STREQ(decoded.pApplicationName, source.pApplicationName);
        EXPECT_EQ(decoded.applicationVersion, source.applicationVersion);
        EXPECT_STREQ(decoded.pEngineName, source.pEngineName);
        EXPECT_EQ(decoded.engineVersion, source.engineVersion);
        EXPECT_EQ(decoded.apiVersion, source.apiVersion);
    }

    TEST(VulkanMarshalTest, InstanceCreateInfoRoundTripWithArraysAndNestedStruct)
    {
        VkApplicationInfo app_info{};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = "app";
        app_info.apiVersion = VK_API_VERSION_1_1;

        const std::array<const char*, 1> layers = {"VK_LAYER_KHRONOS_validation"};
        const std::array<const char*, 2> extensions = {"VK_KHR_surface", "VK_KHR_win32_surface"};

        VkInstanceCreateInfo source{};
        source.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        source.pApplicationInfo = &app_info;
        source.enabledLayerCount = static_cast<uint32_t>(layers.size());
        source.ppEnabledLayerNames = layers.data();
        source.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        source.ppEnabledExtensionNames = extensions.data();

        std::vector<std::byte> buffer;
        marshal::writer writer{buffer};
        marshal::encode(writer, source);

        marshal::reader reader{buffer.data(), buffer.size()};
        marshal::arena arena;
        VkInstanceCreateInfo decoded{};
        ASSERT_TRUE(marshal::decode(reader, arena, decoded));

        EXPECT_EQ(decoded.sType, source.sType);

        ASSERT_NE(decoded.pApplicationInfo, nullptr);
        EXPECT_STREQ(decoded.pApplicationInfo->pApplicationName, "app");
        EXPECT_EQ(decoded.pApplicationInfo->apiVersion, VK_API_VERSION_1_1);

        ASSERT_EQ(decoded.enabledLayerCount, 1u);
        EXPECT_STREQ(decoded.ppEnabledLayerNames[0], "VK_LAYER_KHRONOS_validation");

        ASSERT_EQ(decoded.enabledExtensionCount, 2u);
        EXPECT_STREQ(decoded.ppEnabledExtensionNames[0], "VK_KHR_surface");
        EXPECT_STREQ(decoded.ppEnabledExtensionNames[1], "VK_KHR_win32_surface");
    }
}
