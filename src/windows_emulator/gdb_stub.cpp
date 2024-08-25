#include "std_include.hpp"
#include "gdb_stub.hpp"

#include <utils/finally.hpp>

extern "C" {
#include <gdbstub.h>
}

namespace
{
	gdb_action_t map_gdb_action(const gdb_action action)
	{
		switch (action)
		{
		case gdb_action::none:
			return ACT_NONE;
		case gdb_action::resume:
			return ACT_RESUME;
		case gdb_action::shutdown:
			return ACT_SHUTDOWN;
		}

		throw std::runtime_error("Bad action");
	}

	gdb_stub_handler& get_handler(void* args)
	{
		return *static_cast<gdb_stub_handler*>(args);
	}

	gdb_action_t cont(void* args)
	{
		return map_gdb_action(get_handler(args).cont());
	}

	gdb_action_t stepi(void* args)
	{
		return map_gdb_action(get_handler(args).stepi());
	}

	int read_reg(void* args, const int regno, size_t* value)
	{
		return get_handler(args).read_reg(regno, value) ? 0 : 1;
	}

	int write_reg(void* args, const int regno, const size_t value)
	{
		return get_handler(args).write_reg(regno, value) ? 0 : 1;
	}

	int read_mem(void* args, const size_t addr, const size_t len, void* val)
	{
		return get_handler(args).read_mem(addr, len, val) ? 0 : 1;
	}

	int write_mem(void* args, const size_t addr, const size_t len, void* val)
	{
		return get_handler(args).write_mem(addr, len, val) ? 0 : 1;
	}

	bool set_bp(void* args, const size_t addr, const bp_type_t type)
	{
		(void)type;
		assert(type == BP_SOFTWARE);

		return get_handler(args).set_bp(addr);
	}

	bool del_bp(void* args, const size_t addr, const bp_type_t type)
	{
		(void)type;
		assert(type == BP_SOFTWARE);

		return get_handler(args).del_bp(addr);
	}

	void on_interrupt(void* args)
	{
		get_handler(args).on_interrupt();
	}

	target_ops get_target_ops()
	{
		target_ops ops{};

		ops.cont = cont;
		ops.stepi = stepi;
		ops.read_reg = read_reg;
		ops.write_reg = write_reg;
		ops.read_mem = read_mem;
		ops.write_mem = write_mem;
		ops.set_bp = set_bp;
		ops.del_bp = del_bp;
		ops.on_interrupt = on_interrupt;

		return ops;
	}
}

bool run_gdb_stub(gdb_stub_handler& handler, std::string target_description, const size_t register_count,
                  std::string bind_address)
{
	const arch_info_t info
	{
		target_description.data(),
		static_cast<int>(register_count),
		sizeof(uint64_t),
	};

	auto ops = get_target_ops();

	gdbstub_t stub{};

	if (!gdbstub_init(&stub, &ops, info, bind_address.data()))
	{
		return false;
	}

	const auto _ = utils::finally([&]
	{
		gdbstub_close(&stub);
	});

	return gdbstub_run(&stub, &handler);
}
