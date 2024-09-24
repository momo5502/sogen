#pragma once
#include "x64_emulator.hpp"

namespace context_frame
{
	void save(x64_emulator& emu, CONTEXT& context);
	void restore(x64_emulator& emu, const CONTEXT& context);
}
