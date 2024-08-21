#pragma once

#ifdef _WIN32
#pragma warning(push)
#pragma warning(disable: 4005)
#pragma warning(disable: 4127)
#pragma warning(disable: 4244)
#pragma warning(disable: 4245)
#pragma warning(disable: 4324)
#pragma warning(disable: 4458)
#pragma warning(disable: 4471)
#pragma warning(disable: 4505)
#pragma warning(disable: 4702)
#pragma warning(disable: 4996)
#pragma warning(disable: 5054)
#pragma warning(disable: 6011)
#pragma warning(disable: 6297)
#pragma warning(disable: 6385)
#pragma warning(disable: 6386)
#pragma warning(disable: 6387)
#pragma warning(disable: 26110)
#pragma warning(disable: 26451)
#pragma warning(disable: 26444)
#pragma warning(disable: 26451)
#pragma warning(disable: 26489)
#pragma warning(disable: 26495)
#pragma warning(disable: 26498)
#pragma warning(disable: 26812)
#pragma warning(disable: 28020)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <map>
#include <set>
#include <list>
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
#include <condition_variable>

#include <cassert>

#include <unicorn/unicorn.h>

#define NTDDI_WIN11_GE 0
#define PHNT_VERSION PHNT_WIN11
#include <phnt_windows.h>
#include <phnt.h>

#ifdef _WIN32
#pragma warning(pop)
#endif

#ifdef max
#undef max
#endif

#ifdef min
#undef min
#endif
#endif

using namespace std::literals;
