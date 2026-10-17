#pragma once

#include <array>

#include "target_description.hpp"
#include "osabi_template.hpp"

namespace sogen
{

    // Standard GDB aarch64 core and FP/SIMD features; the register order matches aarch64_gdb_registers.
    inline constexpr auto aarch64_target_descriptions = std::to_array<target_description>({
        {
            .name = "target.xml",
            .xml = R"__xml__(<?xml version="1.0"?>
<!DOCTYPE target SYSTEM "gdb-target.dtd">
<target>
  <architecture>aarch64</architecture>
  )__xml__" OSABI_TEMPLATE R"__xml__(
  <feature name="org.gnu.gdb.aarch64.core">
    <reg name="x0" bitsize="64" type="int" regnum="0"/>
    <reg name="x1" bitsize="64" type="int" regnum="1"/>
    <reg name="x2" bitsize="64" type="int" regnum="2"/>
    <reg name="x3" bitsize="64" type="int" regnum="3"/>
    <reg name="x4" bitsize="64" type="int" regnum="4"/>
    <reg name="x5" bitsize="64" type="int" regnum="5"/>
    <reg name="x6" bitsize="64" type="int" regnum="6"/>
    <reg name="x7" bitsize="64" type="int" regnum="7"/>
    <reg name="x8" bitsize="64" type="int" regnum="8"/>
    <reg name="x9" bitsize="64" type="int" regnum="9"/>
    <reg name="x10" bitsize="64" type="int" regnum="10"/>
    <reg name="x11" bitsize="64" type="int" regnum="11"/>
    <reg name="x12" bitsize="64" type="int" regnum="12"/>
    <reg name="x13" bitsize="64" type="int" regnum="13"/>
    <reg name="x14" bitsize="64" type="int" regnum="14"/>
    <reg name="x15" bitsize="64" type="int" regnum="15"/>
    <reg name="x16" bitsize="64" type="int" regnum="16"/>
    <reg name="x17" bitsize="64" type="int" regnum="17"/>
    <reg name="x18" bitsize="64" type="int" regnum="18"/>
    <reg name="x19" bitsize="64" type="int" regnum="19"/>
    <reg name="x20" bitsize="64" type="int" regnum="20"/>
    <reg name="x21" bitsize="64" type="int" regnum="21"/>
    <reg name="x22" bitsize="64" type="int" regnum="22"/>
    <reg name="x23" bitsize="64" type="int" regnum="23"/>
    <reg name="x24" bitsize="64" type="int" regnum="24"/>
    <reg name="x25" bitsize="64" type="int" regnum="25"/>
    <reg name="x26" bitsize="64" type="int" regnum="26"/>
    <reg name="x27" bitsize="64" type="int" regnum="27"/>
    <reg name="x28" bitsize="64" type="int" regnum="28"/>
    <reg name="x29" bitsize="64" type="int" regnum="29"/>
    <reg name="x30" bitsize="64" type="int" regnum="30"/>
    <reg name="sp" bitsize="64" type="data_ptr"/>
    <reg name="pc" bitsize="64" type="code_ptr"/>
    <reg name="cpsr" bitsize="32" type="int"/>
  </feature>
  <feature name="org.gnu.gdb.aarch64.fpu">
    <vector id="v2d" type="ieee_double" count="2"/>
    <vector id="v2u" type="uint64" count="2"/>
    <vector id="v4f" type="ieee_single" count="4"/>
    <vector id="v4u" type="uint32" count="4"/>
    <vector id="v8u" type="uint16" count="8"/>
    <vector id="v16u" type="uint8" count="16"/>
    <union id="aarch64v">
      <field name="d" type="v2d"/>
      <field name="u" type="v2u"/>
      <field name="s" type="v4f"/>
      <field name="w" type="v4u"/>
      <field name="h" type="v8u"/>
      <field name="b" type="v16u"/>
      <field name="q" type="uint128"/>
    </union>
    <reg name="v0" bitsize="128" type="aarch64v"/>
    <reg name="v1" bitsize="128" type="aarch64v"/>
    <reg name="v2" bitsize="128" type="aarch64v"/>
    <reg name="v3" bitsize="128" type="aarch64v"/>
    <reg name="v4" bitsize="128" type="aarch64v"/>
    <reg name="v5" bitsize="128" type="aarch64v"/>
    <reg name="v6" bitsize="128" type="aarch64v"/>
    <reg name="v7" bitsize="128" type="aarch64v"/>
    <reg name="v8" bitsize="128" type="aarch64v"/>
    <reg name="v9" bitsize="128" type="aarch64v"/>
    <reg name="v10" bitsize="128" type="aarch64v"/>
    <reg name="v11" bitsize="128" type="aarch64v"/>
    <reg name="v12" bitsize="128" type="aarch64v"/>
    <reg name="v13" bitsize="128" type="aarch64v"/>
    <reg name="v14" bitsize="128" type="aarch64v"/>
    <reg name="v15" bitsize="128" type="aarch64v"/>
    <reg name="v16" bitsize="128" type="aarch64v"/>
    <reg name="v17" bitsize="128" type="aarch64v"/>
    <reg name="v18" bitsize="128" type="aarch64v"/>
    <reg name="v19" bitsize="128" type="aarch64v"/>
    <reg name="v20" bitsize="128" type="aarch64v"/>
    <reg name="v21" bitsize="128" type="aarch64v"/>
    <reg name="v22" bitsize="128" type="aarch64v"/>
    <reg name="v23" bitsize="128" type="aarch64v"/>
    <reg name="v24" bitsize="128" type="aarch64v"/>
    <reg name="v25" bitsize="128" type="aarch64v"/>
    <reg name="v26" bitsize="128" type="aarch64v"/>
    <reg name="v27" bitsize="128" type="aarch64v"/>
    <reg name="v28" bitsize="128" type="aarch64v"/>
    <reg name="v29" bitsize="128" type="aarch64v"/>
    <reg name="v30" bitsize="128" type="aarch64v"/>
    <reg name="v31" bitsize="128" type="aarch64v"/>
    <reg name="fpsr" bitsize="32" type="int"/>
    <reg name="fpcr" bitsize="32" type="int"/>
  </feature>
</target>
)__xml__",
        },
    });

} // namespace sogen
