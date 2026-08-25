#define SEVEN_EMULATOR_IMPL
#include "seven_x86_64_emulator.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "uint_wide.h"

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif
#if !defined(_MSC_VER) && (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>
#endif

#include <iced_x86/decoder.hpp>
#include <iced_x86/instruction_info.hpp>
#include <iced_x86/memory_size_info.hpp>
#include <iced_x86/op_access.hpp>

#include <seven/compat.hpp>
#include <seven/handler_helpers.hpp>
#include <seven/x87_encoding.hpp>
#include <seven_jit/jit_executor.hpp>
#include <utils/object.hpp>

namespace seven_backend {

using namespace sogen;

namespace {

#ifndef SOGEN_SEVEN_GIT_HASH
#define SOGEN_SEVEN_GIT_HASH "unknown"
#endif

#ifndef SOGEN_SEVEN_GIT_DIRTY
#define SOGEN_SEVEN_GIT_DIRTY "unknown"
#endif

#ifndef SOGEN_SEVEN_BUILD_STAMP
#define SOGEN_SEVEN_BUILD_STAMP "unknown"
#endif

std::string seven_backend_identity() {
  std::string id = "seven";
  id += " (git:";
  id += SOGEN_SEVEN_GIT_HASH;
  id += ",";
  id += SOGEN_SEVEN_GIT_DIRTY;
  id += ",stamp:";
  id += SOGEN_SEVEN_BUILD_STAMP;
  id += ",built:";
  id += __DATE__;
  id += " ";
  id += __TIME__;
  id += ")";
  return id;
}

bool seven_backend_verbose_enabled() {
  static const bool enabled = [] {
    if (const char* v = std::getenv("SEVEN_BACKEND_VERBOSE")) {
      return v[0] != '\0' && v[0] != '0';
    }
    return false;
  }();
  return enabled;
}

struct hook_object : utils::object {
  enum class kind { executor, memory, local };
  kind hook_kind{kind::local};
  seven::Executor::HookId executor_id{};
  seven::Memory::HookId memory_id{};
};

seven::MemoryPermissionMask to_seven_permissions(memory_permission permissions) {
  seven::MemoryPermissionMask result{};
  if (is_readable(permissions)) result |= static_cast<seven::MemoryPermissionMask>(seven::MemoryPermission::read);
  if (is_writable(permissions)) result |= static_cast<seven::MemoryPermissionMask>(seven::MemoryPermission::write);
  if (is_executable(permissions)) result |= static_cast<seven::MemoryPermissionMask>(seven::MemoryPermission::execute);
  return result;
}

bool map_instruction_trap(int instruction_type, seven::TrapKind& kind) {
  switch (static_cast<x86_hookable_instructions>(instruction_type)) {
    case x86_hookable_instructions::invalid: kind = seven::TrapKind::invalid_opcode; return true;
    case x86_hookable_instructions::syscall: kind = seven::TrapKind::syscall; return true;
    case x86_hookable_instructions::cpuid: kind = seven::TrapKind::cpuid; return true;
    case x86_hookable_instructions::rdtsc: kind = seven::TrapKind::rdtsc; return true;
    case x86_hookable_instructions::rdtscp: kind = seven::TrapKind::rdtscp; return true;
    default: return false;
  }
}

void emulate_cpuid_defaults(seven::CpuState& state) {
  const auto leaf = static_cast<std::uint32_t>(state.gpr[0] & 0xFFFFFFFFull);
  const auto subleaf = static_cast<std::uint32_t>(state.gpr[1] & 0xFFFFFFFFull);
  std::uint32_t eax = 0;
  std::uint32_t ebx = 0;
  std::uint32_t ecx = 0;
  std::uint32_t edx = 0;

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
  {
    int cpu_info[4] = {0, 0, 0, 0};
    __cpuidex(cpu_info, static_cast<int>(leaf), static_cast<int>(subleaf));
    eax = static_cast<std::uint32_t>(cpu_info[0]);
    ebx = static_cast<std::uint32_t>(cpu_info[1]);
    ecx = static_cast<std::uint32_t>(cpu_info[2]);
    edx = static_cast<std::uint32_t>(cpu_info[3]);
  }
#elif !defined(_MSC_VER) && (defined(__x86_64__) || defined(__i386__))
  {
    unsigned int a = 0;
    unsigned int b = 0;
    unsigned int c = 0;
    unsigned int d = 0;
    if (__get_cpuid_count(leaf, subleaf, &a, &b, &c, &d) != 0) {
      eax = static_cast<std::uint32_t>(a);
      ebx = static_cast<std::uint32_t>(b);
      ecx = static_cast<std::uint32_t>(c);
      edx = static_cast<std::uint32_t>(d);
    }
  }
#endif

  // Keep a deterministic fallback profile on platforms where host CPUID is unavailable.
  if (eax == 0 && ebx == 0 && ecx == 0 && edx == 0) {
  if (leaf == 0) {
    eax = 1;
    ebx = 0x756E6547;
    edx = 0x49656E69;
    ecx = 0x6C65746E;
  } else if (leaf == 1) {
    eax = 0x000306A9;
    ebx = 0x00100800;
    ecx = 0;
    edx = 0x0183F3FF;
  } else if (leaf == 7 && subleaf == 0) {
    eax = 0;
    ebx = 0;
    ecx = 0;
    edx = 0;
  }
  }

  state.gpr[0] = eax;
  state.gpr[3] = ebx;
  state.gpr[1] = ecx;
  state.gpr[2] = edx;
}

bool ranges_overlap(uint64_t a_base, uint64_t a_size, uint64_t b_base, uint64_t b_size) {
  if (a_size == 0 || b_size == 0) return false;
  return a_base < (b_base + b_size) && b_base < (a_base + a_size);
}

struct decoded_memory_access {
  uint64_t address{};
  size_t size{};
  bool is_read{};
  bool is_write{};
};

struct violation_info {
  size_t size{};
  memory_operation operation{memory_operation::none};
  memory_violation_type type{memory_violation_type::unmapped};
};

struct basic_block_hook_state {
  bool first = true;
  bool fallthrough_valid = false;
  std::uint64_t fallthrough_rip = 0;
};

bool op_access_reads(const iced_x86::OpAccess access) {
  switch (access) {
    case iced_x86::OpAccess::READ:
    case iced_x86::OpAccess::COND_READ:
    case iced_x86::OpAccess::READ_WRITE:
    case iced_x86::OpAccess::READ_COND_WRITE:
      return true;
    default:
      return false;
  }
}

bool op_access_writes(const iced_x86::OpAccess access) {
  switch (access) {
    case iced_x86::OpAccess::WRITE:
    case iced_x86::OpAccess::COND_WRITE:
    case iced_x86::OpAccess::READ_WRITE:
    case iced_x86::OpAccess::READ_COND_WRITE:
      return true;
    default:
      return false;
  }
}

seven::MemoryPermissionMask operation_to_permissions(const memory_operation op) {
  switch (op) {
    case memory_operation::read:
      return static_cast<seven::MemoryPermissionMask>(seven::MemoryPermission::read);
    case memory_operation::write:
      return static_cast<seven::MemoryPermissionMask>(seven::MemoryPermission::write);
    case memory_operation::exec:
      return static_cast<seven::MemoryPermissionMask>(seven::MemoryPermission::execute);
    default:
      return 0;
  }
}

std::optional<int> interrupt_vector_from_instruction(const seven::TrapHookContext& ctx) {
  switch (ctx.instr.code()) {
    case iced_x86::Code::INT1: return 1;
    case iced_x86::Code::INT3: return 3;
    case iced_x86::Code::INT_IMM8: return static_cast<int>(ctx.instr.immediate8());
    case iced_x86::Code::INTO:
      if ((ctx.state.rflags & seven::kFlagOF) != 0) return 4;
      return std::nullopt;
    default:
      return std::nullopt;
  }
}

std::optional<iced_x86::Instruction> decode_instruction_at(const seven::CpuState& state, const seven::Memory& memory, const uint64_t rip) {
  std::array<uint8_t, iced_x86::IcedConstants::_MAX_INSTRUCTION_LENGTH> bytes{};
  if (!memory.read(rip, bytes.data(), bytes.size(), seven::MemoryAccessKind::instruction_fetch)) {
    return std::nullopt;
  }
  iced_x86::Decoder decoder(seven::decoder_bitness(state.mode), std::span<const uint8_t>(bytes.data(), bytes.size()), rip);
  const auto decoded = decoder.decode();
  if (!decoded.has_value() || decoded->code() == iced_x86::Code::INVALID) {
    return std::nullopt;
  }
  return decoded.value();
}

basic_block summarize_basic_block(const seven::CpuState& state, const seven::Memory& memory, const std::uint64_t rip) {
  basic_block block{};
  block.address = rip;

  std::uint64_t cursor = rip;
  for (std::size_t i = 0; i < 256; ++i) {
    const auto instr = decode_instruction_at(state, memory, cursor);
    if (!instr.has_value()) {
      break;
    }

    const auto length = std::max<std::size_t>(1u, static_cast<std::size_t>(instr->length()));
    block.instruction_count += 1;
    block.size += length;

    const auto flow = iced_x86::InstructionExtensions::flow_control(instr.value());
    if (flow != iced_x86::FlowControl::NEXT) {
      break;
    }

    cursor += length;
  }

  if (block.instruction_count == 0) {
    block.instruction_count = 1;
    block.size = 1;
  }

  return block;
}

std::vector<decoded_memory_access> decode_memory_accesses(seven::CpuState& state, const iced_x86::Instruction& instr) {
  std::vector<decoded_memory_access> accesses{};
  iced_x86::InstructionInfoFactory info_factory{};
  const auto& info = info_factory.info(instr);

  for (const auto& used_mem : info.used_memory()) {
    const auto is_read = op_access_reads(used_mem.access);
    const auto is_write = op_access_writes(used_mem.access);
    if (!is_read && !is_write) {
      continue;
    }

    uint64_t address{};
    if (used_mem.base != iced_x86::Register::NONE) {
      address += seven::detail::read_register(state, used_mem.base);
    }
    if (used_mem.index != iced_x86::Register::NONE) {
      address += seven::detail::read_register(state, used_mem.index) * used_mem.scale;
    }
    address += used_mem.displacement;
    if (used_mem.segment == iced_x86::Register::FS) {
      address += state.fs_base;
    } else if (used_mem.segment == iced_x86::Register::GS) {
      address += state.gs_base;
    }
    address = seven::mask_linear_address(state, address);

    const auto size = std::max<size_t>(1, iced_x86::memory_size_ext::get_size(used_mem.memory_size));
    accesses.push_back(decoded_memory_access{
      .address = address,
      .size = size,
      .is_read = is_read,
      .is_write = is_write,
    });
  }

  return accesses;
}

violation_info classify_violation(const seven::FaultHookEvent& event) {
  violation_info out{};
  out.size = 1;

  if (event.result.reason != seven::StopReason::page_fault && event.result.reason != seven::StopReason::general_protection) {
    return out;
  }

  const auto maybe_instr = decode_instruction_at(event.state, event.memory, event.instruction_rip);
  if (maybe_instr.has_value()) {
    const auto accesses = decode_memory_accesses(event.state, maybe_instr.value());
    for (const auto& access : accesses) {
      if (!ranges_overlap(access.address, access.size, event.fault_address, 1)) {
        continue;
      }
      out.size = access.size;
      if (access.is_write) out.operation = memory_operation::write;
      else if (access.is_read) out.operation = memory_operation::read;
      break;
    }
  }

  if (out.operation == memory_operation::none) {
    if (event.fault_address == event.instruction_rip) {
      out.operation = memory_operation::exec;
    }
  }

  if (!event.memory.is_mapped(event.fault_address, std::max<size_t>(1, out.size))) {
    out.type = memory_violation_type::unmapped;
    return out;
  }

  const auto required = operation_to_permissions(out.operation);
  if (required != 0 && !event.memory.has_permissions(event.fault_address, std::max<size_t>(1, out.size), required)) {
    out.type = memory_violation_type::protection;
  } else {
    // Includes hook-denied accesses that are mapped and permissioned.
    out.type = memory_violation_type::protection;
  }

  return out;
}

size_t gpr_index(x86_register reg) {
  switch (reg) {
    case x86_register::rax: case x86_register::eax: case x86_register::ax: case x86_register::al: case x86_register::ah: return 0;
    case x86_register::rcx: case x86_register::ecx: case x86_register::cx: case x86_register::cl: case x86_register::ch: return 1;
    case x86_register::rdx: case x86_register::edx: case x86_register::dx: case x86_register::dl: case x86_register::dh: return 2;
    case x86_register::rbx: case x86_register::ebx: case x86_register::bx: case x86_register::bl: case x86_register::bh: return 3;
    case x86_register::rsp: case x86_register::esp: case x86_register::sp: case x86_register::spl: return 4;
    case x86_register::rbp: case x86_register::ebp: case x86_register::bp: case x86_register::bpl: return 5;
    case x86_register::rsi: case x86_register::esi: case x86_register::si: case x86_register::sil: return 6;
    case x86_register::rdi: case x86_register::edi: case x86_register::di: case x86_register::dil: return 7;
    case x86_register::r8: case x86_register::r8d: case x86_register::r8w: case x86_register::r8b: return 8;
    case x86_register::r9: case x86_register::r9d: case x86_register::r9w: case x86_register::r9b: return 9;
    case x86_register::r10: case x86_register::r10d: case x86_register::r10w: case x86_register::r10b: return 10;
    case x86_register::r11: case x86_register::r11d: case x86_register::r11w: case x86_register::r11b: return 11;
    case x86_register::r12: case x86_register::r12d: case x86_register::r12w: case x86_register::r12b: return 12;
    case x86_register::r13: case x86_register::r13d: case x86_register::r13w: case x86_register::r13b: return 13;
    case x86_register::r14: case x86_register::r14d: case x86_register::r14w: case x86_register::r14b: return 14;
    case x86_register::r15: case x86_register::r15d: case x86_register::r15w: case x86_register::r15b: return 15;
    default: throw std::runtime_error("unsupported gpr");
  }
}

bool is_high8(x86_register reg) {
  return reg == x86_register::ah || reg == x86_register::bh || reg == x86_register::ch || reg == x86_register::dh;
}

size_t gpr_width(x86_register reg) {
  switch (reg) {
    case x86_register::al: case x86_register::ah: case x86_register::bl: case x86_register::bh:
    case x86_register::cl: case x86_register::ch: case x86_register::dl: case x86_register::dh:
    case x86_register::spl: case x86_register::bpl: case x86_register::sil: case x86_register::dil:
    case x86_register::r8b: case x86_register::r9b: case x86_register::r10b: case x86_register::r11b:
    case x86_register::r12b: case x86_register::r13b: case x86_register::r14b: case x86_register::r15b: return 1;
    case x86_register::ax: case x86_register::bx: case x86_register::cx: case x86_register::dx:
    case x86_register::sp: case x86_register::bp: case x86_register::si: case x86_register::di:
    case x86_register::r8w: case x86_register::r9w: case x86_register::r10w: case x86_register::r11w:
    case x86_register::r12w: case x86_register::r13w: case x86_register::r14w: case x86_register::r15w: return 2;
    case x86_register::eax: case x86_register::ebx: case x86_register::ecx: case x86_register::edx:
    case x86_register::esp: case x86_register::ebp: case x86_register::esi: case x86_register::edi:
    case x86_register::r8d: case x86_register::r9d: case x86_register::r10d: case x86_register::r11d:
    case x86_register::r12d: case x86_register::r13d: case x86_register::r14d: case x86_register::r15d: return 4;
    default: return 8;
  }
}

bool try_vector_index_width(x86_register reg, size_t& index, size_t& width) {
  const auto value = static_cast<int>(reg);
  const auto xmm0 = static_cast<int>(x86_register::xmm0);
  const auto ymm0 = static_cast<int>(x86_register::ymm0);
  const auto zmm0 = static_cast<int>(x86_register::zmm0);
  if (value >= xmm0 && value < xmm0 + 32) { index = static_cast<size_t>(value - xmm0); width = 16; return true; }
  if (value >= ymm0 && value < ymm0 + 32) { index = static_cast<size_t>(value - ymm0); width = 32; return true; }
  if (value >= zmm0 && value < zmm0 + 32) { index = static_cast<size_t>(value - zmm0); width = 64; return true; }
  return false;
}

bool resolve_dr_index_for_api(const seven::CpuState& state, x86_register reg, size_t& index) {
  if (reg < x86_register::dr0 || reg > x86_register::dr7) {
    return false;
  }
  index = static_cast<size_t>(static_cast<int>(reg) - static_cast<int>(x86_register::dr0));
  if (index == 4 || index == 5) {
    // SDM: with CR4.DE=1, DR4/5 access is undefined for MOV DR and treated as #UD.
    // For API-level register access, refuse these IDs when DE is set.
    if ((state.cr[4] & (1ull << 3)) != 0) {
      return false;
    }
    index += 2;
  }
  return index < 8;
}

class seven_x86_64_emulator final : public x86_64_emulator {
 public:
  seven_x86_64_emulator() {
    static const bool trace_testproject = [] {
      if (const char* v = std::getenv("SEVEN_TRACE_TESTPROJECT")) {
        return v[0] != '\0' && v[0] != '0';
      }
      return false;
    }();
    if (trace_testproject) {
      testproject_trace_hook_id_ = executor_.interpreter().add_instruction_hook([](seven::InstructionHookContext& ctx) {
        constexpr std::uint64_t kStart = 0x1400058A0ull;
        constexpr std::uint64_t kEnd = 0x140006000ull;
        if (ctx.state.rip >= kStart && ctx.state.rip < kEnd) {
          std::fprintf(stderr,
                       "[seven-testproject] rip=0x%llx code=%u len=%u rax=0x%llx rcx=0x%llx rdx=0x%llx rflags=0x%llx\n",
                       static_cast<unsigned long long>(ctx.state.rip),
                       static_cast<unsigned>(ctx.instr.code()),
                       static_cast<unsigned>(ctx.instr.length()),
                       static_cast<unsigned long long>(ctx.state.gpr[0]),
                       static_cast<unsigned long long>(ctx.state.gpr[1]),
                       static_cast<unsigned long long>(ctx.state.gpr[2]),
                       static_cast<unsigned long long>(ctx.state.rflags));
        }
        return seven::InstructionHookResult{};
      });
    }

    static const bool trace_unique_instructions = [] {
      if (const char* v = std::getenv("SEVEN_TRACE_UNIQUE_INSTRUCTIONS")) {
        return v[0] != '\0' && v[0] != '0';
      }
      return false;
    }();
    if (trace_unique_instructions) {
      unique_instruction_trace_enabled_ = true;
      unique_instruction_trace_hook_id_ = executor_.interpreter().add_instruction_hook([this](seven::InstructionHookContext& ctx) {
        const auto code = static_cast<std::uint32_t>(ctx.instr.code());
        if (unique_instruction_ids_.find(code) == unique_instruction_ids_.end()) {
          const auto mnemonic = static_cast<std::uint32_t>(ctx.instr.mnemonic());
          unique_instruction_ids_.emplace(code, mnemonic);
          std::fprintf(stderr, "[seven-unique] code=%u mnemonic=%u\n",
                       static_cast<unsigned>(code),
                       static_cast<unsigned>(mnemonic));
        }
        return seven::InstructionHookResult{};
      });
    }
  }

  ~seven_x86_64_emulator() override {
    if (!unique_instruction_trace_enabled_) {
      return;
    }
    std::fprintf(stderr, "[seven-unique] count=%zu\n", unique_instruction_ids_.size());
    for (const auto& [code, mnemonic] : unique_instruction_ids_) {
      std::fprintf(stderr, "[seven-unique] code=%u mnemonic=%u\n",
                   static_cast<unsigned>(code),
                   static_cast<unsigned>(mnemonic));
    }
  }

  void start(const size_t count) override {
    if (seven_backend_verbose_enabled()) {
      std::fprintf(stderr, "[seven-backend] start(count=%zu) rip=0x%llx\n",
                   count,
                   static_cast<unsigned long long>(state_.rip));
    }
    executor_.interpreter().clear_stop_request();
    if (count == 0) {
      // Run unbounded execution in slices so request_stop() can preempt
      // thread spins (windows_emulator uses stop() to yield on timeslice).
      constexpr size_t kInstructionSlice = 0x20000;
      while (true) {
        try {
          last_result_ = executor_.run(state_, memory_, kInstructionSlice);
        } catch (...) {
          throw;
        }
        if (seven_backend_verbose_enabled()) {
          std::fprintf(stderr,
                       "[seven-backend] slice reason=%u retired=%zu rip=0x%llx\n",
                       static_cast<unsigned>(last_result_.reason),
                       last_result_.retired,
                       static_cast<unsigned long long>(state_.rip));
        }
        if (last_result_.reason == seven::StopReason::execution_limit) {
          continue;
        }
        break;
      }
      return;
    }
    try {
      last_result_ = executor_.run(state_, memory_, count);
    } catch (...) {
      throw;
    }
    if (seven_backend_verbose_enabled()) {
      std::fprintf(stderr,
                   "[seven-backend] done reason=%u retired=%zu rip=0x%llx\n",
                   static_cast<unsigned>(last_result_.reason),
                   last_result_.retired,
                   static_cast<unsigned long long>(state_.rip));
    }
  }

  void stop() override {
    if (seven_backend_verbose_enabled()) {
      std::fprintf(stderr, "[seven-backend] stop() requested\n");
    }
    executor_.interpreter().request_stop();
  }

  void load_gdt(pointer_type address, uint32_t limit) override {
    state_.gdtr.base = address;
    state_.gdtr.limit = static_cast<uint16_t>(limit);
  }

  void set_segment_base(x86_register base, pointer_type value) override {
    if (base == x86_register::fs_base || base == x86_register::fs) state_.fs_base = value;
    if (base == x86_register::gs_base || base == x86_register::gs) state_.gs_base = value;
  }

  pointer_type get_segment_base(x86_register base) override {
    if (base == x86_register::fs_base || base == x86_register::fs) return state_.fs_base;
    if (base == x86_register::gs_base || base == x86_register::gs) return state_.gs_base;
    return 0;
  }

  size_t write_raw_register(int reg, const void* value, size_t size) override {
    if (!value || size == 0) return 0;
    const auto r = static_cast<x86_register>(reg);
    uint64_t scalar{};
    std::memcpy(&scalar, value, std::min(size, sizeof(scalar)));

    if (r == x86_register::rip || r == x86_register::eip || r == x86_register::ip) {
      if (r == x86_register::rip) state_.rip = scalar;
      else if (r == x86_register::eip) state_.rip = (state_.rip & ~0xFFFFFFFFull) | (scalar & 0xFFFFFFFFull);
      else state_.rip = (state_.rip & ~0xFFFFull) | (scalar & 0xFFFFull);
      return std::min(size, r == x86_register::rip ? size_t(8) : (r == x86_register::eip ? size_t(4) : size_t(2)));
    }
    if (r == x86_register::rflags || r == x86_register::flags || r == x86_register::eflags) {
      if (r == x86_register::eflags) {
        state_.rflags = (state_.rflags & ~0xFFFFFFFFull) | (scalar & 0xFFFFFFFFull);
        return std::min(size, size_t(4));
      }
      state_.rflags = scalar;
      return std::min(size, size_t(8));
    }
    if (r == x86_register::es || r == x86_register::cs || r == x86_register::ss || r == x86_register::ds || r == x86_register::fs || r == x86_register::gs) {
      size_t idx = 0;
      if (r == x86_register::es) idx = 0;
      if (r == x86_register::cs) idx = 1;
      if (r == x86_register::ss) idx = 2;
      if (r == x86_register::ds) idx = 3;
      if (r == x86_register::fs) idx = 4;
      if (r == x86_register::gs) idx = 5;
      state_.sreg[idx] = static_cast<uint16_t>(scalar);
      return std::min(size, size_t(2));
    }
    if (r == x86_register::fs_base) { state_.fs_base = scalar; return std::min(size, size_t(8)); }
    if (r == x86_register::gs_base) { state_.gs_base = scalar; return std::min(size, size_t(8)); }
    if (r == x86_register::mxcsr) { state_.mxcsr = static_cast<uint32_t>(scalar); return std::min(size, size_t(4)); }
    if (r == x86_register::fpcw) { state_.x87_control_word = static_cast<uint16_t>(scalar); return std::min(size, size_t(2)); }
    if (r == x86_register::fpsw) { state_.set_x87_status_word(static_cast<uint16_t>(scalar)); return std::min(size, size_t(2)); }
    if (r == x86_register::fptag) {
      const auto abridged = static_cast<std::uint8_t>(scalar & 0xFFu);
      for (size_t i = 0; i < 8; ++i) {
        state_.x87_tags[i] = ((abridged >> i) & 1u) != 0 ? 0x0 : 0x3;
      }
      return std::min(size, size_t(2));
    }
    if (r == x86_register::gdtr) { state_.gdtr.base = scalar; return std::min(size, size_t(8)); }
    if (r == x86_register::idtr) { state_.idtr.base = scalar; return std::min(size, size_t(8)); }
    if (r >= x86_register::st0 && r <= x86_register::st7) {
      const auto idx = static_cast<size_t>(static_cast<int>(r) - static_cast<int>(x86_register::st0));
      std::array<std::uint8_t, 16> raw{};
      const auto write_size = std::min(size, raw.size());
      std::memcpy(raw.data(), value, write_size);
      state_.x87_stack[idx] = seven::handlers::x87_encoding::decode_ext80(raw.data());
      state_.x87_tags[idx] = (state_.x87_stack[idx] == 0) ? 0x1 : 0x0;
      return write_size;
    }
    if (r >= x86_register::cr0 && r <= x86_register::cr8) {
      const auto idx = static_cast<size_t>(static_cast<int>(r) - static_cast<int>(x86_register::cr0));
      if (idx >= state_.cr.size()) return 0;
      state_.cr[idx] = scalar;
      return std::min(size, size_t(8));
    }

    size_t dr_idx{};
    if (resolve_dr_index_for_api(state_, r, dr_idx)) {
      if (dr_idx >= state_.dr.size()) return 0;
      state_.dr[dr_idx] = scalar;
      return std::min(size, size_t(8));
    }
    if (r >= x86_register::dr0 && r <= x86_register::dr7) {
      return 0;
    }

    if (r >= x86_register::mm0 && r <= x86_register::mm7) {
      const auto idx = static_cast<size_t>(static_cast<int>(r) - static_cast<int>(x86_register::mm0));
      state_.mmx[idx] = scalar;
      return std::min(size, size_t(8));
    }

    size_t vec_index{};
    size_t vec_width{};
    if (try_vector_index_width(r, vec_index, vec_width)) {
      std::array<uint8_t, 64> bytes{};
      math::wide_integer::export_bits(state_.vectors[vec_index].value, bytes.begin(), 8, false);
      const auto write_size = std::min(size, vec_width);
      std::memcpy(bytes.data(), value, write_size);
      seven::SimdUint out{};
      math::wide_integer::import_bits(out, bytes.begin(), bytes.end(), 8, false);
      state_.vectors[vec_index].value = out;
      return write_size;
    }

    try {
      const auto idx = gpr_index(r);
      const auto width = gpr_width(r);
      auto& gpr = state_.gpr[idx];
      if (width == 8) gpr = scalar;
      else if (width == 4) gpr = (scalar & 0xFFFFFFFFull);
      else if (width == 2) gpr = (gpr & ~0xFFFFull) | (scalar & 0xFFFFull);
      else if (is_high8(r)) gpr = (gpr & ~0xFF00ull) | ((scalar & 0xFFull) << 8);
      else gpr = (gpr & ~0xFFull) | (scalar & 0xFFull);
      return std::min(size, width);
    } catch (...) {
      return 0;
    }
  }

  size_t read_raw_register(int reg, void* value, size_t size) override {
    if (!value || size == 0) return 0;
    const auto r = static_cast<x86_register>(reg);
    uint64_t scalar{};
    size_t out_size{};

    if (r == x86_register::rip || r == x86_register::eip || r == x86_register::ip) {
      scalar = state_.rip;
      out_size = (r == x86_register::rip ? 8u : (r == x86_register::eip ? 4u : 2u));
    } else if (r == x86_register::rflags || r == x86_register::flags || r == x86_register::eflags) {
      scalar = state_.rflags;
      out_size = (r == x86_register::eflags ? 4u : 8u);
    } else if (r == x86_register::es || r == x86_register::cs || r == x86_register::ss || r == x86_register::ds || r == x86_register::fs || r == x86_register::gs) {
      size_t idx = 0;
      if (r == x86_register::es) idx = 0;
      if (r == x86_register::cs) idx = 1;
      if (r == x86_register::ss) idx = 2;
      if (r == x86_register::ds) idx = 3;
      if (r == x86_register::fs) idx = 4;
      if (r == x86_register::gs) idx = 5;
      scalar = state_.sreg[idx];
      out_size = 2;
    } else if (r == x86_register::fs_base) {
      scalar = state_.fs_base;
      out_size = 8;
    } else if (r == x86_register::gs_base) {
      scalar = state_.gs_base;
      out_size = 8;
    } else if (r == x86_register::mxcsr) {
      scalar = state_.mxcsr;
      out_size = 4;
    } else if (r == x86_register::fpcw) {
      scalar = state_.x87_control_word;
      out_size = 2;
    } else if (r == x86_register::fpsw) {
      scalar = state_.x87_status_word;
      out_size = 2;
    } else if (r == x86_register::fptag) {
      std::uint8_t abridged = 0;
      for (size_t i = 0; i < 8; ++i) {
        if (state_.x87_tags[i] != 0x3) abridged |= static_cast<std::uint8_t>(1u << i);
      }
      scalar = abridged;
      out_size = 1;
    } else if (r == x86_register::gdtr) {
      scalar = state_.gdtr.base;
      out_size = 8;
    } else if (r == x86_register::idtr) {
      scalar = state_.idtr.base;
      out_size = 8;
    } else if (r >= x86_register::st0 && r <= x86_register::st7) {
      const auto idx = static_cast<size_t>(static_cast<int>(r) - static_cast<int>(x86_register::st0));
      std::array<std::uint8_t, 16> raw{};
      seven::handlers::x87_encoding::encode_ext80(state_.x87_stack[idx], raw.data());
      const auto read_size = std::min(size, raw.size());
      std::memcpy(value, raw.data(), read_size);
      return read_size;
    }
    else if (r >= x86_register::cr0 && r <= x86_register::cr8) {
      const auto idx = static_cast<size_t>(static_cast<int>(r) - static_cast<int>(x86_register::cr0));
      if (idx >= state_.cr.size()) return 0;
      scalar = state_.cr[idx];
      out_size = 8;
    } else {
      size_t dr_idx{};
      if (resolve_dr_index_for_api(state_, r, dr_idx)) {
        if (dr_idx >= state_.dr.size()) return 0;
        scalar = state_.dr[dr_idx];
        out_size = 8;
      } else if (r >= x86_register::dr0 && r <= x86_register::dr7) {
        return 0;
      } else if (r >= x86_register::mm0 && r <= x86_register::mm7) {
        const auto idx = static_cast<size_t>(static_cast<int>(r) - static_cast<int>(x86_register::mm0));
        scalar = state_.mmx[idx];
        out_size = 8;
      } else {
        size_t vec_index{};
        size_t vec_width{};
        if (try_vector_index_width(r, vec_index, vec_width)) {
          std::array<uint8_t, 64> bytes{};
          math::wide_integer::export_bits(state_.vectors[vec_index].value, bytes.begin(), 8, false);
          const auto read_size = std::min(size, vec_width);
          std::memcpy(value, bytes.data(), read_size);
          return read_size;
        }
        try {
          scalar = state_.gpr[gpr_index(r)];
          const auto width = gpr_width(r);
          if (width == 4) scalar &= 0xFFFFFFFFull;
          if (width == 2) scalar &= 0xFFFFull;
          if (width == 1) scalar = is_high8(r) ? ((scalar >> 8) & 0xFFull) : (scalar & 0xFFull);
          out_size = width;
        } catch (...) {
          return 0;
        }
      }
    }

    const auto copy_size = std::min(size, out_size);
    std::memcpy(value, &scalar, copy_size);
    return copy_size;
  }

  bool read_descriptor_table(int reg, descriptor_table_register& table) override {
    const auto r = static_cast<x86_register>(reg);
    if (r == x86_register::gdtr) { table.base = state_.gdtr.base; table.limit = state_.gdtr.limit; return true; }
    if (r == x86_register::idtr) { table.base = state_.idtr.base; table.limit = state_.idtr.limit; return true; }
    return false;
  }

  void map_mmio(uint64_t address, size_t size, mmio_read_callback read_cb, mmio_write_callback write_cb) override {
    auto replace_or_add_binding = [&](mmio_read_callback r, mmio_write_callback w) {
      for (auto& binding : mmio_bindings_) {
        if (binding.base == address && binding.size == size) {
          binding.read = std::move(r);
          binding.write = std::move(w);
          return;
        }
      }
      mmio_bindings_.push_back(mmio_binding{
        .base = address,
        .size = size,
        .read = std::move(r),
        .write = std::move(w),
      });
    };
    replace_or_add_binding(read_cb, write_cb);

    (void)memory_.map_mmio(
      address, size,
      [cb = std::move(read_cb)](uint64_t offset, void* dst, size_t cb_size) {
        cb(offset, dst, cb_size);
        return true;
      },
      [cb = std::move(write_cb)](uint64_t offset, const void* src, size_t cb_size) {
        cb(offset, src, cb_size);
        return true;
      });
  }

  void map_memory(uint64_t address, size_t size, memory_permission permissions) override { memory_.map(address, size, to_seven_permissions(permissions)); }
  void unmap_memory(uint64_t address, size_t size) override { memory_.unmap(address, size); }
  bool try_read_memory(uint64_t address, void* data, size_t size) const override { return memory_.read_unchecked(address, data, size); }
  bool try_write_memory(uint64_t address, const void* data, size_t size) override { return memory_.write_unchecked(address, data, size); }

  void read_memory(uint64_t address, void* data, size_t size) const override {
    if (!memory_.read_unchecked(address, data, size)) throw std::runtime_error("read_memory failed");
  }

  void write_memory(uint64_t address, const void* data, size_t size) override {
    if (memory_.write_unchecked(address, data, size)) return;
    // The write landed on an as-yet-uncommitted page (sogen extends an exception/context frame just
    // below the committed stack -- classic stack growth; unicorn/whp accept it implicitly, seven's
    // page model is strict). Commit ONLY the missing pages as RW (never downgrade an existing
    // region's permissions) and retry -- this is the on-demand stack commit a real OS performs.
    const uint64_t P = 0x1000;
    const uint64_t start = address & ~(P - 1);
    const uint64_t end = (address + size + P - 1) & ~(P - 1);
    // Map missing pages RWX: covers on-demand stack growth AND the devirt harness overlaying a warmed
    // process (VM-allocated code/data regions) into a fresh emulator. Permissive perms are correct for
    // faithful analysis -- we never want a spurious DEP fault to derail a trace.
    for (uint64_t p = start; p < end; p += P)
      if (!memory_.is_mapped(p, 1)) memory_.map(p, P, seven::kMemoryPermissionAll);
    if (memory_.write_unchecked(address, data, size)) return;
    if (std::getenv("SEVEN_MEM_DEBUG"))
      std::fprintf(stderr, "[seven] write_memory FAILED (even after commit) addr=0x%llx size=%zu\n",
                   (unsigned long long)address, size);
    throw std::runtime_error("write_memory failed");
  }

  void apply_memory_protection(uint64_t address, size_t size, memory_permission permissions) override {
    memory_.reprotect(address, size, to_seven_permissions(permissions));
  }

  emulator_hook* hook_instruction(int instruction_type, instruction_hook_callback callback) override {
    seven::TrapKind trap{};
    if (!map_instruction_trap(instruction_type, trap)) return make_local_hook();
    const auto id = executor_.interpreter().add_trap_hook(trap, [cb = std::move(callback), this](seven::TrapHookContext& ctx) {
      const auto trap_rip = ctx.state.rip;
      uint64_t data{};
      if (ctx.kind == seven::TrapKind::cpuid) {
        data = static_cast<uint32_t>(ctx.state.gpr[0]);
      } else if (ctx.kind == seven::TrapKind::syscall) {
        data = ctx.state.gpr[0];
      } else if (ctx.kind == seven::TrapKind::invalid_opcode) {
        data = static_cast<uint64_t>(ctx.state.rip);
      }
      seven::TrapHookResult result{};
      const auto action = cb(*this, data);
      if (ctx.kind == seven::TrapKind::cpuid) {
        if (action != instruction_hook_continuation::skip_instruction) {
          emulate_cpuid_defaults(ctx.state);
        }
        result.action = seven::TrapHookAction::handled;
      } else {
        result.action = action == instruction_hook_continuation::skip_instruction
          ? seven::TrapHookAction::handled
          : seven::TrapHookAction::continue_to_core;
      }

      if (ctx.kind == seven::TrapKind::syscall &&
          action == instruction_hook_continuation::skip_instruction &&
          ctx.state.rip != trap_rip) {
        // SOGEN's shared syscall helpers rewrite RIP as if the backend will still
        // advance past the syscall instruction after the hook returns. Unicorn
        // does that for us; seven handles the trap entirely in the hook, so we
        // must apply the same advance here to preserve the existing contract.
        ctx.state.rip += static_cast<std::uint64_t>(ctx.instr.length());
      }
      return result;
    });
    return make_executor_hook(id);
  }

  emulator_hook* hook_basic_block(basic_block_hook_callback callback) override {
    auto state = std::make_shared<basic_block_hook_state>();
    const auto id = executor_.interpreter().add_instruction_hook([cb = std::move(callback), state = std::move(state), this](seven::InstructionHookContext& ctx) {
      const bool block_start = state->first || !state->fallthrough_valid || ctx.state.rip != state->fallthrough_rip;
      if (block_start) {
        cb(*this, summarize_basic_block(ctx.state, ctx.memory, ctx.state.rip));
      }

      state->first = false;
      state->fallthrough_rip = ctx.next_rip;
      state->fallthrough_valid =
          iced_x86::InstructionExtensions::flow_control(ctx.instr) == iced_x86::FlowControl::NEXT;
      return seven::InstructionHookResult{};
    });
    return make_executor_hook(id);
  }

  emulator_hook* hook_interrupt(interrupt_hook_callback callback) override {
    const auto id = executor_.interpreter().add_trap_hook(seven::TrapKind::interrupt, [cb = std::move(callback), this](seven::TrapHookContext& ctx) {
      const auto maybe_vector = interrupt_vector_from_instruction(ctx);
      if (!maybe_vector.has_value()) {
        return seven::TrapHookResult{};
      }
      const auto saved_rip = ctx.state.rip;
      ctx.state.rip = ctx.next_rip;
      cb(*this, maybe_vector.value());
      if (ctx.state.rip == saved_rip) {
        ctx.state.rip = ctx.next_rip;
      }
      seven::TrapHookResult result{};
      result.action = seven::TrapHookAction::handled;
      return result;
    });
    return make_executor_hook(id);
  }

  emulator_hook* hook_memory_violation(memory_violation_hook_callback callback) override {
    const auto id = executor_.interpreter().add_fault_hook([cb = std::move(callback), this](const seven::FaultHookEvent& event) {
      if (event.result.reason != seven::StopReason::page_fault &&
          event.result.reason != seven::StopReason::general_protection) {
        return seven::FaultHookAction::stop;
      }
      const auto info = classify_violation(event);
      const auto old_rip = event.state.rip;
      const auto cont = cb(*this, event.fault_address, info.size, info.operation, info.type);
      if (std::getenv("SEVEN_MEM_DEBUG"))
        std::fprintf(stderr, "[seven] fault rip=0x%llx addr=0x%llx size=%zu op=%d type=%d cont=%d mapped=%d\n",
                     (unsigned long long)old_rip, (unsigned long long)event.fault_address, info.size,
                     (int)info.operation, (int)info.type, (int)cont, (int)memory_.is_mapped(event.fault_address, 1));
      const auto new_rip = this->state_.rip;
      if (cont == memory_violation_continuation::restart) return seven::FaultHookAction::restart_instruction;
      // Fallback on-demand growth -- MUST precede the resume/stop handling below. sogen returns
      // `resume` for a fault it could not grow (it can't grow a reservation its memory_manager doesn't
      // track -- e.g. the warmed VMProtect stack/VM-scratch region the devirt harness restored), which
      // would otherwise loop forever re-faulting. If the page is STILL unmapped after sogen's handler,
      // commit it RWX (zero-filled, matching a fresh grown page) and restart. Covers on-demand growth
      // of both the guest stack and VMProtect's large VM scratch area, for reads and writes alike --
      // a read of a fresh grown page correctly yields zero. (Gated to DEVIRT_AUTOCOMMIT so this
      // permissive analysis behavior never affects a normal sogen run.) The ground-truth diff verifies
      // the zero-fill is correct: a missed-capture region would surface as a later trace divergence.
      if (std::getenv("DEVIRT_AUTOCOMMIT") && info.type == memory_violation_type::unmapped &&
          !memory_.is_mapped(event.fault_address, 1)) {
        const uint64_t P = 0x1000;
        const uint64_t s = event.fault_address & ~(P - 1);
        const uint64_t e = (event.fault_address + std::max<size_t>(static_cast<size_t>(1), info.size) + P - 1) & ~(P - 1);
        for (uint64_t p = s; p < e; p += P)
          if (!memory_.is_mapped(p, 1)) memory_.map(p, P, seven::kMemoryPermissionAll);
        return seven::FaultHookAction::restart_instruction;
      }
      if (cont == memory_violation_continuation::resume) {
        if (new_rip != old_rip) {
          return seven::FaultHookAction::retry;
        }
        return seven::FaultHookAction::retry;
      }
      return seven::FaultHookAction::stop;
    });
    return make_executor_hook(id);
  }

  emulator_hook* hook_memory_range_execution(uint64_t address, uint64_t size, memory_execution_hook_callback callback) override {
    if (address == 0 && size == std::numeric_limits<uint64_t>::max()) {
      const auto id = executor_.interpreter().add_execution_hook([cb = std::move(callback), this](std::uint64_t rip) { cb(*this, rip); });
      return make_executor_hook(id);
    }
    if (size == 1) {
      const auto id = executor_.interpreter().add_execution_hook(address, [cb = std::move(callback), this](std::uint64_t rip) { cb(*this, rip); });
      return make_executor_hook(id);
    }
    const uint64_t end = address + size;
    const auto id = executor_.interpreter().add_execution_hook([cb = std::move(callback), address, end, this](std::uint64_t rip) {
      if (rip >= address && rip < end) cb(*this, rip);
    });
    return make_executor_hook(id);
  }

  emulator_hook* hook_memory_execution(memory_execution_hook_callback callback) override {
    return hook_memory_range_execution(0, std::numeric_limits<uint64_t>::max(), std::move(callback));
  }

  emulator_hook* hook_memory_execution(uint64_t address, memory_execution_hook_callback callback) override {
    return hook_memory_range_execution(address, 1, std::move(callback));
  }

  emulator_hook* hook_memory_read(uint64_t address, uint64_t size, memory_access_hook_callback callback) override {
    const auto id = memory_.add_access_hook(
        [cb = std::move(callback), this](const seven::MemoryAccessEvent& event) {
          cb(*this, event.address, event.data, event.data_size);
          return true;
        },
        seven::MemoryHookRange{.base = address, .size = static_cast<size_t>(size)},
        seven::bit(seven::MemoryAccessKind::data_read));
    return make_memory_hook(id);
  }

  emulator_hook* hook_memory_write(uint64_t address, uint64_t size, memory_access_hook_callback callback) override {
    const auto id = memory_.add_access_hook(
        [cb = std::move(callback), this](const seven::MemoryAccessEvent& event) {
          cb(*this, event.address, event.data, event.data_size);
          return true;
        },
        seven::MemoryHookRange{.base = address, .size = static_cast<size_t>(size)},
        seven::bit(seven::MemoryAccessKind::data_write));
    return make_memory_hook(id);
  }

  void delete_hook(emulator_hook* hook) override {
    const auto* target = reinterpret_cast<hook_object*>(hook);
    auto it = std::find_if(hooks_.begin(), hooks_.end(), [target](const auto& entry) { return entry.get() == target; });
    if (it == hooks_.end()) return;
    if ((*it)->hook_kind == hook_object::kind::executor) (void)executor_.interpreter().remove_hook((*it)->executor_id);
    if ((*it)->hook_kind == hook_object::kind::memory) (void)memory_.remove_access_hook((*it)->memory_id);
    hooks_.erase(it);
  }

  void serialize_state(utils::buffer_serializer& buffer, bool is_snapshot) const override {
    auto regs = save_registers();
    buffer.write_vector(regs);
    if (!is_snapshot) return;

    const auto pages = memory_.snapshot_pages();
    buffer.write<uint64_t>(pages.size());
    for (const auto& page : pages) {
      buffer.write(page.page_index);
      buffer.write(page.permissions);
      buffer.write(page.data.data(), page.data.size());
    }

    const auto mmio_regions = memory_.snapshot_mmio_regions();
    buffer.write<uint64_t>(mmio_regions.size());
    for (const auto& region : mmio_regions) {
      buffer.write(region.id);
      buffer.write(region.base);
      buffer.write(region.size);
    }
  }

  void deserialize_state(utils::buffer_deserializer& buffer, bool is_snapshot) override {
    const auto regs = buffer.read_vector<std::byte>();
    restore_registers(regs);
    if (!is_snapshot) return;

    memory_ = {};
    const auto page_count = buffer.read<uint64_t>();
    std::vector<seven::Memory::PageSnapshot> pages{};
    pages.reserve(static_cast<size_t>(page_count));
    for (uint64_t i = 0; i < page_count; ++i) {
      seven::Memory::PageSnapshot page{};
      buffer.read(page.page_index);
      buffer.read(page.permissions);
      buffer.read(page.data.data(), page.data.size());
      pages.push_back(page);
    }
    memory_.restore_pages(pages);

    const auto mmio_count = buffer.read<uint64_t>();
    std::vector<seven::Memory::MmioRegionSnapshot> regions{};
    regions.reserve(static_cast<size_t>(mmio_count));
    for (uint64_t i = 0; i < mmio_count; ++i) {
      seven::Memory::MmioRegionSnapshot region{};
      buffer.read(region.id);
      buffer.read(region.base);
      buffer.read(region.size);
      regions.push_back(region);
    }

    memory_.restore_mmio_regions(regions, [this](const seven::Memory::MmioRegionSnapshot& region)
        -> std::optional<std::pair<seven::Memory::MmioReadCallback, seven::Memory::MmioWriteCallback>> {
      for (const auto& binding : mmio_bindings_) {
        if (binding.base != region.base || binding.size != region.size) {
          continue;
        }
        seven::Memory::MmioReadCallback read_cb =
            [cb = binding.read](uint64_t offset, void* dst, size_t cb_size) {
              cb(offset, dst, cb_size);
              return true;
            };
        seven::Memory::MmioWriteCallback write_cb =
            [cb = binding.write](uint64_t offset, const void* src, size_t cb_size) {
              cb(offset, src, cb_size);
              return true;
            };
        return std::make_optional(std::make_pair(std::move(read_cb), std::move(write_cb)));
      }
      return std::nullopt;
    });
  }

  std::vector<std::byte> save_registers() const override {
    utils::buffer_serializer s{};
    s.write(static_cast<uint8_t>(state_.mode));
    s.write(state_.rip);
    s.write(state_.rflags);
    s.write(state_.fs_base);
    s.write(state_.gs_base);
    s.write(state_.mxcsr);
    s.write(state_.x87_control_word);
    s.write(state_.x87_status_word);
    s.write(state_.x87_top);
    s.write(state_.gdtr.base);
    s.write(state_.gdtr.limit);
    s.write(state_.idtr.base);
    s.write(state_.idtr.limit);
    for (const auto& v : state_.gpr) s.write(v);
    for (const auto& v : state_.mmx) s.write(v);
    for (const auto& v : state_.sreg) s.write(v);
    for (const auto& v : state_.cr) s.write(v);
    for (const auto& v : state_.dr) s.write(v);
    for (const auto& v : state_.tr) s.write(v);
    for (const auto& v : state_.x87_tags) s.write(v);
    for (const auto& v : state_.xcr) s.write(v);

    s.write<uint64_t>(state_.msr.size());
    for (const auto& [id, val] : state_.msr) {
      s.write(id);
      s.write(val);
    }
    for (const auto& f : state_.x87_stack) {
      std::array<std::uint8_t, 10> raw{};
      seven::handlers::x87_encoding::encode_ext80(f, raw.data());
      s.write(raw.data(), raw.size());
    }

    for (const auto& v : state_.vectors) {
      std::array<uint8_t, 64> bytes{};
      auto vv = v.value;
      math::wide_integer::export_bits(vv, bytes.begin(), 8, false);
      s.write(bytes.data(), bytes.size());
    }

    return s.move_buffer();
  }

  void restore_registers(const std::vector<std::byte>& register_data) override {
    utils::buffer_deserializer d{register_data};
    uint8_t mode{};
    d.read(mode);
    state_.mode = static_cast<seven::ExecutionMode>(mode);
    d.read(state_.rip);
    d.read(state_.rflags);
    d.read(state_.fs_base);
    d.read(state_.gs_base);
    d.read(state_.mxcsr);
    d.read(state_.x87_control_word);
    d.read(state_.x87_status_word);
    d.read(state_.x87_top);
    d.read(state_.gdtr.base);
    d.read(state_.gdtr.limit);
    d.read(state_.idtr.base);
    d.read(state_.idtr.limit);
    for (auto& v : state_.gpr) d.read(v);
    for (auto& v : state_.mmx) d.read(v);
    for (auto& v : state_.sreg) d.read(v);
    for (auto& v : state_.cr) d.read(v);
    for (auto& v : state_.dr) d.read(v);
    for (auto& v : state_.tr) d.read(v);
    for (auto& v : state_.x87_tags) d.read(v);
    for (auto& v : state_.xcr) d.read(v);

    state_.msr.clear();
    const auto msr_count = d.read<uint64_t>();
    for (uint64_t i = 0; i < msr_count; ++i) {
      uint32_t id{};
      uint64_t val{};
      d.read(id);
      d.read(val);
      state_.msr.emplace(id, val);
    }

    for (auto& f : state_.x87_stack) {
      std::array<std::uint8_t, 10> raw{};
      d.read(raw.data(), raw.size());
      f = seven::handlers::x87_encoding::decode_ext80(raw.data());
    }
    for (auto& v : state_.vectors) {
      std::array<uint8_t, 64> bytes{};
      d.read(bytes.data(), bytes.size());
      seven::SimdUint out{};
      math::wide_integer::import_bits(out, bytes.begin(), bytes.end(), 8, false);
      v.value = out;
    }
  }

  bool has_violation() const override { return executor_.interpreter().has_violation(); }
  bool supports_instruction_counting() const override { return true; }
  // seven is an interpreter: stop() must be cooperative from the CPU thread (request_stop sets a
  // flag polled by the run loop), so cross-thread cancellation is not advertised as safe.
  bool is_stop_thread_safe() const override { return false; }
  bool supports_multiple_vcpus() const override { return false; }
  std::string get_name() const override { return seven_backend_identity(); }

 private:
  struct mmio_binding {
    uint64_t base{};
    size_t size{};
    mmio_read_callback read{};
    mmio_write_callback write{};
  };

  emulator_hook* make_executor_hook(seven::Executor::HookId id) {
    auto hook = std::make_unique<hook_object>();
    hook->hook_kind = hook_object::kind::executor;
    hook->executor_id = id;
    auto* ptr = reinterpret_cast<emulator_hook*>(hook.get());
    hooks_.push_back(std::move(hook));
    return ptr;
  }

  emulator_hook* make_memory_hook(seven::Memory::HookId id) {
    auto hook = std::make_unique<hook_object>();
    hook->hook_kind = hook_object::kind::memory;
    hook->memory_id = id;
    auto* ptr = reinterpret_cast<emulator_hook*>(hook.get());
    hooks_.push_back(std::move(hook));
    return ptr;
  }

  emulator_hook* make_local_hook() {
    auto hook = std::make_unique<hook_object>();
    auto* ptr = reinterpret_cast<emulator_hook*>(hook.get());
    hooks_.push_back(std::move(hook));
    return ptr;
  }

  seven::CpuState state_{};
  seven::Memory memory_{};
  // JIT-backed: seven_jit::JitExecutor wraps a plain seven::Executor (reachable via .interpreter())
  // and adds a native-codegen fast path for spans BlockCompiler can cover, falling back to the
  // interpreter one instruction at a time for everything else -- same hook/trap fidelity either way,
  // only throughput on the covered subset changes. run()'s signature is identical to
  // seven::Executor::run()'s, so start()'s executor_.run(...) call below needed no change; every
  // hook-registration/state-query call does, since JitExecutor doesn't re-expose that surface itself
  // (see seven_jit/jit_executor.hpp) -- it goes through executor_.interpreter() instead.
  seven_jit::JitExecutor executor_{};
  seven::ExecutionResult last_result_{};
  std::vector<std::unique_ptr<hook_object>> hooks_{};
  std::vector<mmio_binding> mmio_bindings_{};
  std::optional<seven::Executor::HookId> testproject_trace_hook_id_{};
  bool unique_instruction_trace_enabled_{false};
  std::optional<seven::Executor::HookId> unique_instruction_trace_hook_id_{};
  std::map<std::uint32_t, std::uint32_t> unique_instruction_ids_{};
};

} // namespace

std::unique_ptr<x86_64_emulator> create_x86_64_emulator() {
  return std::make_unique<seven_x86_64_emulator>();
}

} // namespace seven_backend
