#include "std_include.hpp"
#include "unicorn.hpp"

unicorn::unicorn(const uc_arch arch, const uc_mode mode)
{
	const auto error = uc_open(arch, mode, &this->uc_);
	ThrowIfUnicornError(error);
}

unicorn::unicorn(unicorn&& obj) noexcept
{
	this->operator=(std::move(obj));
}

unicorn& unicorn::operator=(unicorn&& obj) noexcept
{
	if (this != &obj)
	{
		this->close();

		this->uc_ = obj.uc_;
		obj.uc_ = nullptr;
	}

	return *this;
}

unicorn::~unicorn()
{
	this->close();
}

void unicorn::close()
{
	if (this->uc_)
	{
		uc_close(this->uc_);
		this->uc_ = nullptr;
	}
}
