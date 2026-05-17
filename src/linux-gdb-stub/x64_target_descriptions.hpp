#pragma once

#include <map>
#include <string>

inline std::map<std::string, std::string, std::less<>> x64_target_descriptions{
    {
        "target.xml",
        R"__xml__(<?xml version="1.0"?>
<!-- Copyright (C) 2010-2018 Free Software Foundation, Inc.

     Copying and distribution of this file, with or without modification,
     are permitted in any medium without royalty provided the copyright
     notice and this notice are preserved.  -->

<!-- AMD64 with AVX -->

<!DOCTYPE target SYSTEM "gdb-target.dtd">
<target>
  <architecture>i386:x86-64</architecture>
  <xi:include href="64bit-core.xml"/>
  <xi:include href="64bit-sse.xml"/>
  <xi:include href="64bit-segments.xml"/>
  <xi:include href="64bit-avx.xml"/>
</target>
)__xml__",
    },
    {
        "64bit-core.xml",
        R"__xml__(<?xml version="1.0"?>
<!-- Copyright (C) 2010-2018 Free Software Foundation, Inc.

     Copying and distribution of this file, with or without modification,
     are permitted in any medium without royalty provided the copyright
     notice and this notice are preserved.  -->

<!DOCTYPE feature SYSTEM "gdb-target.dtd">
<feature name="org.gnu.gdb.i386.core">
  <flags id="i386_eflags" size="4">
    <field name="CF" start="0" end="0"/>
    <field name="" start="1" end="1"/>
    <field name="PF" start="2" end="2"/>
    <field name="AF" start="4" end="4"/>
    <field name="ZF" start="6" end="6"/>
    <field name="SF" start="7" end="7"/>
    <field name="TF" start="8" end="8"/>
    <field name="IF" start="9" end="9"/>
    <field name="DF" start="10" end="10"/>
    <field name="OF" start="11" end="11"/>
    <field name="NT" start="14" end="14"/>
    <field name="RF" start="16" end="16"/>
    <field name="VM" start="17" end="17"/>
    <field name="AC" start="18" end="18"/>
    <field name="VIF" start="19" end="19"/>
    <field name="VIP" start="20" end="20"/>
    <field name="ID" start="21" end="21"/>
  </flags>

  <reg name="rax" bitsize="64" type="int64"/>
  <reg name="rbx" bitsize="64" type="int64"/>
  <reg name="rcx" bitsize="64" type="int64"/>
  <reg name="rdx" bitsize="64" type="int64"/>
  <reg name="rsi" bitsize="64" type="int64"/>
  <reg name="rdi" bitsize="64" type="int64"/>
  <reg name="rbp" bitsize="64" type="data_ptr"/>
  <reg name="rsp" bitsize="64" type="data_ptr"/>
  <reg name="r8" bitsize="64" type="int64"/>
  <reg name="r9" bitsize="64" type="int64"/>
  <reg name="r10" bitsize="64" type="int64"/>
  <reg name="r11" bitsize="64" type="int64"/>
  <reg name="r12" bitsize="64" type="int64"/>
  <reg name="r13" bitsize="64" type="int64"/>
  <reg name="r14" bitsize="64" type="int64"/>
  <reg name="r15" bitsize="64" type="int64"/>

  <reg name="rip" bitsize="64" type="code_ptr"/>
  <reg name="eflags" bitsize="32" type="i386_eflags"/>
  <reg name="cs" bitsize="32" type="int32"/>
  <reg name="ss" bitsize="32" type="int32"/>
  <reg name="ds" bitsize="32" type="int32"/>
  <reg name="es" bitsize="32" type="int32"/>
  <reg name="fs" bitsize="32" type="int32"/>
  <reg name="gs" bitsize="32" type="int32"/>

  <reg name="st0" bitsize="80" type="i387_ext"/>
  <reg name="st1" bitsize="80" type="i387_ext"/>
  <reg name="st2" bitsize="80" type="i387_ext"/>
  <reg name="st3" bitsize="80" type="i387_ext"/>
  <reg name="st4" bitsize="80" type="i387_ext"/>
  <reg name="st5" bitsize="80" type="i387_ext"/>
  <reg name="st6" bitsize="80" type="i387_ext"/>
  <reg name="st7" bitsize="80" type="i387_ext"/>

  <reg name="fctrl" bitsize="32" type="int" group="float"/>
  <reg name="fstat" bitsize="32" type="int" group="float"/>
  <reg name="ftag" bitsize="32" type="int" group="float"/>
  <reg name="fiseg" bitsize="32" type="int" group="float"/>
  <reg name="fioff" bitsize="32" type="int" group="float"/>
  <reg name="foseg" bitsize="32" type="int" group="float"/>
  <reg name="fooff" bitsize="32" type="int" group="float"/>
  <reg name="fop" bitsize="32" type="int" group="float"/>
</feature>
)__xml__",
    },
    {
        "64bit-sse.xml",
        R"__xml__(<?xml version="1.0"?>
<!-- Copyright (C) 2010-2018 Free Software Foundation, Inc.

     Copying and distribution of this file, with or without modification,
     are permitted in any medium without royalty provided the copyright
     notice and this notice are preserved.  -->

<!DOCTYPE feature SYSTEM "gdb-target.dtd">
<feature name="org.gnu.gdb.i386.sse">
  <vector id="v4f" type="ieee_single" count="4"/>
  <vector id="v2d" type="ieee_double" count="2"/>
  <vector id="v16i8" type="int8" count="16"/>
  <vector id="v8i16" type="int16" count="8"/>
  <vector id="v4i32" type="int32" count="4"/>
  <vector id="v2i64" type="int64" count="2"/>
  <union id="vec128">
    <field name="v4_float" type="v4f"/>
    <field name="v2_double" type="v2d"/>
    <field name="v16_int8" type="v16i8"/>
    <field name="v8_int16" type="v8i16"/>
    <field name="v4_int32" type="v4i32"/>
    <field name="v2_int64" type="v2i64"/>
    <field name="uint128" type="uint128"/>
  </union>
  <flags id="i386_mxcsr" size="4">
    <field name="IE" start="0" end="0"/>
    <field name="DE" start="1" end="1"/>
    <field name="ZE" start="2" end="2"/>
    <field name="OE" start="3" end="3"/>
    <field name="UE" start="4" end="4"/>
    <field name="PE" start="5" end="5"/>
    <field name="DAZ" start="6" end="6"/>
    <field name="IM" start="7" end="7"/>
    <field name="DM" start="8" end="8"/>
    <field name="ZM" start="9" end="9"/>
    <field name="OM" start="10" end="10"/>
    <field name="UM" start="11" end="11"/>
    <field name="PM" start="12" end="12"/>
    <field name="FZ" start="15" end="15"/>
  </flags>

  <reg name="xmm0" bitsize="128" type="vec128" regnum="40"/>
  <reg name="xmm1" bitsize="128" type="vec128"/>
  <reg name="xmm2" bitsize="128" type="vec128"/>
  <reg name="xmm3" bitsize="128" type="vec128"/>
  <reg name="xmm4" bitsize="128" type="vec128"/>
  <reg name="xmm5" bitsize="128" type="vec128"/>
  <reg name="xmm6" bitsize="128" type="vec128"/>
  <reg name="xmm7" bitsize="128" type="vec128"/>
  <reg name="xmm8" bitsize="128" type="vec128"/>
  <reg name="xmm9" bitsize="128" type="vec128"/>
  <reg name="xmm10" bitsize="128" type="vec128"/>
  <reg name="xmm11" bitsize="128" type="vec128"/>
  <reg name="xmm12" bitsize="128" type="vec128"/>
  <reg name="xmm13" bitsize="128" type="vec128"/>
  <reg name="xmm14" bitsize="128" type="vec128"/>
  <reg name="xmm15" bitsize="128" type="vec128"/>

  <reg name="mxcsr" bitsize="32" type="i386_mxcsr" group="vector"/>
</feature>
)__xml__",
    },
    {
        "64bit-segments.xml",
        R"__xml__(<?xml version="1.0"?>
<!-- Copyright (C) 2016-2018 Free Software Foundation, Inc.

     Copying and distribution of this file, with or without modification,
     are permitted in any medium without royalty provided the copyright
     notice and this notice are preserved.  -->

<!DOCTYPE feature SYSTEM "gdb-target.dtd">
<feature name="org.gnu.gdb.i386.segments">
  <reg name="fs_base" bitsize="64" type="int"/>
  <reg name="gs_base" bitsize="64" type="int"/>
</feature>
)__xml__",
    },
    {
        "64bit-avx.xml",
        R"__xml__(<?xml version="1.0"?>
<!-- Copyright (C) 2010-2018 Free Software Foundation, Inc.

     Copying and distribution of this file, with or without modification,
     are permitted in any medium without royalty provided the copyright
     notice and this notice are preserved.  -->

<!DOCTYPE feature SYSTEM "gdb-target.dtd">
<feature name="org.gnu.gdb.i386.avx">
  <reg name="ymm0h" bitsize="128" type="uint128"/>
  <reg name="ymm1h" bitsize="128" type="uint128"/>
  <reg name="ymm2h" bitsize="128" type="uint128"/>
  <reg name="ymm3h" bitsize="128" type="uint128"/>
  <reg name="ymm4h" bitsize="128" type="uint128"/>
  <reg name="ymm5h" bitsize="128" type="uint128"/>
  <reg name="ymm6h" bitsize="128" type="uint128"/>
  <reg name="ymm7h" bitsize="128" type="uint128"/>
  <reg name="ymm8h" bitsize="128" type="uint128"/>
  <reg name="ymm9h" bitsize="128" type="uint128"/>
  <reg name="ymm10h" bitsize="128" type="uint128"/>
  <reg name="ymm11h" bitsize="128" type="uint128"/>
  <reg name="ymm12h" bitsize="128" type="uint128"/>
  <reg name="ymm13h" bitsize="128" type="uint128"/>
  <reg name="ymm14h" bitsize="128" type="uint128"/>
  <reg name="ymm15h" bitsize="128" type="uint128"/>
</feature>
)__xml__",
    },
};
