#include <cstdint>
#include <cstring>
#include <string>
#include <fstream>
#include <thread>
#include <atomic>
#include <vector>
#include <Windows.h>

// getenv is broken right now :(
std::string read_env(const char* env)
{
	char buffer[0x1000] = {};
	if (!GetEnvironmentVariableA(env, buffer, sizeof(buffer)))
	{
		return {};
	}

	return buffer;
}

bool test_threads()
{
	constexpr auto thread_count = 5ULL;

	std::atomic<uint64_t> counter{0};

	std::vector<std::thread> threads{};
	threads.reserve(thread_count);

	for (auto i = 0ULL; i < thread_count; ++i)
	{
		threads.emplace_back([&counter]
		{
			++counter;
			std::this_thread::yield();
			++counter;
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			++counter;
		});
	}

	for (auto& t : threads)
	{
		t.join();
	}

	return counter == (thread_count * 3ULL);
}

bool test_env()
{
	const auto computername = read_env("COMPUTERNAME");

	SetEnvironmentVariableA("BLUB", "LUL");

	const auto blub = read_env("BLUB");

	return !computername.empty() && blub == "LUL";
}

bool test_io()
{
	const auto* filename = "a.txt";

	FILE* fp{};
	(void)fopen_s(&fp, filename, "wb");

	if (!fp)
	{
		puts("Bad file");
		return false;
	}

	const std::string text = "Blub";

	(void)fwrite(text.data(), 1, text.size(), fp);
	(void)fclose(fp);

	std::ifstream t(filename);
	t.seekg(0, std::ios::end);
	const size_t size = t.tellg();
	std::string buffer(size, ' ');
	t.seekg(0);
	t.read(buffer.data(), static_cast<std::streamsize>(size));

	return text == buffer;
}

#define RUN_TEST(func, name)         \
{                                    \
	printf("Running test '" name "': "); \
	const auto res = func();         \
	valid &= res;                    \
	puts(res ? "Sucess" : "Fail");   \
}

int main(int /*argc*/, const char* /*argv*/[])
{
	bool valid = true;

	RUN_TEST(test_io, "I/O")
	RUN_TEST(test_threads, "Threads")
	RUN_TEST(test_env, "Environment")

	return valid ? 0 : 1;
}
