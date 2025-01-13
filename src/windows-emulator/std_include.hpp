#pragma once

#ifdef _WIN32
#pragma warning(push)
#pragma warning(disable : 4005)
#pragma warning(disable : 4127)
#pragma warning(disable : 4201)
#pragma warning(disable : 4244)
#pragma warning(disable : 4245)
#pragma warning(disable : 4324)
#pragma warning(disable : 4458)
#pragma warning(disable : 4471)
#pragma warning(disable : 4505)
#pragma warning(disable : 4702)
#pragma warning(disable : 4996)
#pragma warning(disable : 5054)
#pragma warning(disable : 6011)
#pragma warning(disable : 6297)
#pragma warning(disable : 6385)
#pragma warning(disable : 6386)
#pragma warning(disable : 6387)
#pragma warning(disable : 26110)
#pragma warning(disable : 26451)
#pragma warning(disable : 26444)
#pragma warning(disable : 26451)
#pragma warning(disable : 26489)
#pragma warning(disable : 26495)
#pragma warning(disable : 26498)
#pragma warning(disable : 26812)
#pragma warning(disable : 28020)
#pragma warning(pop)
#endif

#include <map>
#include <set>
#include <list>
#include <span>
#include <array>
#include <deque>
#include <queue>
#include <thread>
#include <ranges>
#include <atomic>
#include <vector>
#include <mutex>
#include <string>
#include <chrono>
#include <memory>
#include <fstream>
#include <functional>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
#include <condition_variable>

#include <cassert>
#include <cstdarg>
#include <cinttypes>

#include "platform/platform.hpp"

using namespace std::literals;
