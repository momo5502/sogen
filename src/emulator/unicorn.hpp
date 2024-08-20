#pragma once

struct unicorn_error : std::runtime_error
{
	unicorn_error(const uc_err error_code)
		: std::runtime_error(uc_strerror(error_code))
		  , code(error_code)
	{
	}

	uc_err code{};
};

inline void ThrowIfUnicornError(const uc_err error_code)
{
	if (error_code != UC_ERR_OK)
	{
		throw unicorn_error(error_code);
	}
}

#define uce ThrowIfUnicornError

class unicorn
{
public:
	unicorn() = default;
	unicorn(uc_arch arch, uc_mode mode);

	unicorn(const unicorn& obj) = delete;
	unicorn& operator=(const unicorn& obj) = delete;

	unicorn(unicorn&& obj) noexcept;
	unicorn& operator=(unicorn&& obj) noexcept;

	~unicorn();

	void close();

	operator uc_engine*() const
	{
		return this->uc_;
	}

	template <typename T = uint64_t>
	T reg(const int regid) const
	{
		T value{};
		uce(uc_reg_read(this->uc_, regid, &value));
		return value;
	}

	template <typename T = uint64_t, typename S>
	void reg(const int regid, const S& maybe_value) const
	{
		T value = static_cast<T>(maybe_value);
		uce(uc_reg_write(this->uc_, regid, &value));
	}

	void stop() const
	{
		uce(uc_emu_stop(this->uc_));
	}

	uint64_t read_stack(const size_t index) const
	{
		uint64_t result{};
		const auto rsp = this->reg(UC_X86_REG_RSP);

		uce(uc_mem_read(this->uc_, rsp + (index * sizeof(result)), &result, sizeof(result)));
		return result;
	}

private:
	uc_engine* uc_{};
};
