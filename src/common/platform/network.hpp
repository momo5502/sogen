#pragma once

template <typename Traits>
struct EMU_WSABUF
{
    ULONG len;
    EMULATOR_CAST(typename Traits::PVOID, CHAR*) buf;
};
