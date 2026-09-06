// MIT License © 2026 Binary Dice Games
/// @file win32_debug_backend.cpp
/// @brief Implementation of win32_debug_backend. See the header and
///        DESIGN.md §3/§7 for the design rationale; this file is the only
///        place in the dbg module that touches the debuggee process,
///        memory, registers, or the OS debug event stream (DESIGN.md §7).
#include "win32_debug_backend.hpp"

#if defined(_WIN32)

#include <algorithm>
#include <cstring>
#include <future>

#pragma comment(lib, "dbghelp.lib")

namespace bdg::wish::dbg {

namespace {

#if defined(_M_X64) || defined(_M_ARM64)
constexpr DWORD kMachineType = IMAGE_FILE_MACHINE_AMD64;
DWORD64 ctx_pc(const CONTEXT& c) {
  return c.Rip;
}
void set_ctx_pc(CONTEXT& c, DWORD64 v) {
  c.Rip = v;
}
DWORD64 ctx_sp(const CONTEXT& c) {
  return c.Rsp;
}
DWORD64 ctx_fp(const CONTEXT& c) {
  return c.Rbp;
}
#else
constexpr DWORD kMachineType = IMAGE_FILE_MACHINE_I386;
DWORD64 ctx_pc(const CONTEXT& c) {
  return c.Eip;
}
void set_ctx_pc(CONTEXT& c, DWORD64 v) {
  c.Eip = static_cast<DWORD>(v);
}
DWORD64 ctx_sp(const CONTEXT& c) {
  return c.Esp;
}
DWORD64 ctx_fp(const CONTEXT& c) {
  return c.Ebp;
}
#endif

struct line_search_state {
  const std::string* want_file;
  int want_line;
  DWORD64 best_addr{0};
  int best_line{-1};
};

// Free function (not a lambda) so its calling convention matches
// PSYM_ENUMLINES_CALLBACK (CALLBACK == __stdcall) unambiguously.
BOOL CALLBACK enum_lines_cb(PSRCCODEINFO info, PVOID user) {
  auto* st = static_cast<line_search_state*>(user);
  std::string fname = info->FileName;
  const std::string& want = *st->want_file;

  bool match = fname.size() >= want.size() &&
      fname.compare(fname.size() - want.size(), want.size(), want) == 0;
  if (!match) {
    auto base_pos = want.find_last_of("/\\");
    std::string want_base = base_pos == std::string::npos ? want : want.substr(base_pos + 1);
    auto fbase_pos = fname.find_last_of("/\\");
    std::string have_base = fbase_pos == std::string::npos ? fname : fname.substr(fbase_pos + 1);
    match = have_base == want_base;
  }
  if (!match)
    return TRUE;

  int line_no = static_cast<int>(info->LineNumber);
  if (line_no == st->want_line) {
    st->best_addr = info->Address;
    st->best_line = line_no;
    return FALSE; // Exact match -- stop enumerating.
  }
  if (line_no > st->want_line && (st->best_line == -1 || line_no < st->best_line)) {
    st->best_addr = info->Address;
    st->best_line = line_no;
  }
  return TRUE;
}

// x86/x64 CV register codes used by SYMBOL_INFO::Register for
// SYMFLAG_REGREL locals (evaluate()'s address computation below). These
// IDs are a stable part of the Microsoft symbol-format ABI, also listed in
// the DIA SDK's cvconst.h -- which ships only with Visual Studio, not the
// public Windows SDK's dbghelp.h, hence hardcoded here rather than
// #included. ARM64 is treated like x64 (frame/stack pointer relative),
// matching how ctx_fp()/ctx_sp() above already fold _M_ARM64 into the
// _M_X64 branch; a real ARM64 CV register mapping is future work.
#if defined(_M_X64) || defined(_M_ARM64)
constexpr ULONG kCvRegFramePtr = 334; // CV_AMD64_RBP
constexpr ULONG kCvRegStackPtr = 335; // CV_AMD64_RSP
#else
constexpr ULONG kCvRegFramePtr = 22; // CV_REG_EBP
constexpr ULONG kCvRegStackPtr = 21; // CV_REG_ESP
#endif

// DbgHelp's BasicType enum (also from cvconst.h -- see note above); the
// only base types evaluate()'s scalar formatting understands. Anything
// else (structs, arrays, pointers-to-non-scalar, ...) is reported as a raw
// hex byte dump instead of a decoded value -- see DESIGN.md §1's Watch
// scope limitation ("simple local variable reads", not full expression
// evaluation).
enum basic_type {
  kBtChar = 2,
  kBtWChar = 3,
  kBtInt = 6,
  kBtUInt = 7,
  kBtFloat = 8,
  kBtBool = 10,
};

std::string format_scalar(DWORD base_type, size_t size, const unsigned char* buf) {
  switch (base_type) {
    case kBtBool:
      return buf[0] ? "true" : "false";
    case kBtChar:
    case kBtWChar:
      return std::string(1, static_cast<char>(buf[0]));
    case kBtInt: {
      int64_t v = 0;
      if (size == 1)
        v = static_cast<int8_t>(buf[0]);
      else if (size == 2) {
        int16_t t;
        std::memcpy(&t, buf, sizeof(t));
        v = t;
      } else if (size == 4) {
        int32_t t;
        std::memcpy(&t, buf, sizeof(t));
        v = t;
      } else if (size >= 8) {
        int64_t t;
        std::memcpy(&t, buf, sizeof(t));
        v = t;
      }
      return std::to_string(v);
    }
    case kBtUInt: {
      uint64_t v = 0;
      if (size == 1)
        v = buf[0];
      else if (size == 2) {
        uint16_t t;
        std::memcpy(&t, buf, sizeof(t));
        v = t;
      } else if (size == 4) {
        uint32_t t;
        std::memcpy(&t, buf, sizeof(t));
        v = t;
      } else if (size >= 8) {
        uint64_t t;
        std::memcpy(&t, buf, sizeof(t));
        v = t;
      }
      return std::to_string(v);
    }
    case kBtFloat:
      if (size == 4) {
        float f;
        std::memcpy(&f, buf, sizeof(f));
        return std::to_string(f);
      }
      if (size == 8) {
        double d;
        std::memcpy(&d, buf, sizeof(d));
        return std::to_string(d);
      }
      return "<float>";
    default: {
      static const char* kHex = "0123456789abcdef";
      std::string s = "0x";
      for (size_t i = size; i-- > 0;) {
        s += kHex[(buf[i] >> 4) & 0xF];
        s += kHex[buf[i] & 0xF];
      }
      return s;
    }
  }
}

// TI_GET_SYMNAME only names types that have their own symbol record
// (structs/classes/enums/typedefs) -- intrinsic base types like `int` have
// none, so it fails for the common case of a plain scalar local. Fall back
// to synthesizing a name from TI_GET_BASETYPE/TI_GET_LENGTH for those.
std::string base_type_name(DWORD base_type, ULONG64 length) {
  switch (base_type) {
    case kBtBool:
      return "bool";
    case kBtChar:
      return length >= 2 ? "wchar_t" : "char";
    case kBtWChar:
      return "wchar_t";
    case kBtInt:
      switch (length) {
        case 1: return "char";
        case 2: return "short";
        case 8: return "int64";
        default: return "int";
      }
    case kBtUInt:
      switch (length) {
        case 1: return "unsigned char";
        case 2: return "unsigned short";
        case 8: return "uint64";
        default: return "unsigned int";
      }
    case kBtFloat:
      return length == 8 ? "double" : "float";
    default:
      return "";
  }
}

struct watch_eval_state {
  const std::vector<std::string>* names;
  HANDLE process;
  DWORD64 frame_ptr;
  DWORD64 stack_ptr;
  std::vector<watch_entry>* out;
};

// Free function (not a lambda) so its calling convention matches
// PSYM_ENUMERATESYMBOLS_CALLBACK (CALLBACK == __stdcall) unambiguously --
// same reasoning as enum_lines_cb above.
BOOL CALLBACK enum_locals_cb(PSYMBOL_INFO sym, ULONG /*size*/, PVOID user) {
  auto* st = static_cast<watch_eval_state*>(user);
  std::string name(sym->Name, sym->NameLen);
  bool wanted = false;
  for (auto& n : *st->names) {
    if (n == name) {
      wanted = true;
      break;
    }
  }
  if (!wanted)
    return TRUE;

  watch_entry entry;
  entry.name = name;

  DWORD base_type = 0;
  SymGetTypeInfo(st->process, sym->ModBase, sym->TypeIndex, TI_GET_BASETYPE, &base_type);
  ULONG64 length = sym->Size ? sym->Size : 8;

  WCHAR* type_name_w = nullptr;
  if (SymGetTypeInfo(st->process, sym->ModBase, sym->TypeIndex, TI_GET_SYMNAME, &type_name_w) && type_name_w) {
    int len = WideCharToMultiByte(CP_UTF8, 0, type_name_w, -1, nullptr, 0, nullptr, nullptr);
    if (len > 1) {
      std::string tn(static_cast<size_t>(len - 1), '\0');
      WideCharToMultiByte(CP_UTF8, 0, type_name_w, -1, tn.data(), len, nullptr, nullptr);
      entry.type = std::move(tn);
    }
    LocalFree(type_name_w);
  }
  // Intrinsic base types (int, bool, float, ...) have no symbol record of
  // their own, so TI_GET_SYMNAME above fails/returns empty for the common
  // case of a plain scalar local -- synthesize a name from the base type
  // instead of leaving it blank.
  if (entry.type.empty())
    entry.type = base_type_name(base_type, length);

  DWORD64 addr = 0;
  if (sym->Flags & SYMFLAG_REGREL) {
    DWORD64 base = sym->Register == kCvRegStackPtr ? st->stack_ptr : st->frame_ptr;
    addr = base + static_cast<DWORD64>(sym->Address);
  } else if (!(sym->Flags & SYMFLAG_REGISTER)) {
    addr = sym->Address; // Absolute (static/global-scoped) address.
  }
  // SYMFLAG_REGISTER locals live only in a register at this point in the
  // program (optimized code), not in memory -- not resolvable from a
  // stack-frame snapshot alone, so addr stays 0 and is reported below as
  // unavailable rather than silently wrong (DESIGN.md §1).

  if (addr == 0) {
    entry.value = "<unavailable>";
  } else {
    unsigned char buf[8]{};
    SIZE_T to_read = static_cast<SIZE_T>(std::min<ULONG64>(length, sizeof(buf)));
    SIZE_T bytes_read = 0;
    if (to_read > 0 &&
        ReadProcessMemory(st->process, reinterpret_cast<LPCVOID>(addr), buf, to_read, &bytes_read) &&
        bytes_read == to_read) {
      entry.value = format_scalar(base_type, to_read, buf);
    } else {
      entry.value = "<unreadable>";
    }
  }

  st->out->push_back(std::move(entry));
  return TRUE;
}

} // namespace

win32_debug_backend::~win32_debug_backend() {
  detach();
  if (sym_initialized_ && process_)
    SymCleanup(process_);
}

bool win32_debug_backend::attach(uint32_t pid) {
  if (attached_)
    return false;
  pid_ = pid;
  stop_requested_ = false;
  resume_signaled_ = false;
  pending_step_ = pending_step{};

  auto started = std::make_shared<std::promise<bool>>();
  std::future<bool> fut = started->get_future();
  debug_thread_ = std::thread([this, pid, started]() {
    if (!DebugActiveProcess(pid)) {
      started->set_value(false);
      return;
    }
    attached_ = true;
    started->set_value(true);
    debug_thread_main(pid);
  });

  bool ok = fut.get();
  if (!ok && debug_thread_.joinable())
    debug_thread_.join();
  return ok;
}

void win32_debug_backend::detach() {
  if (!debug_thread_.joinable())
    return;
  stop_requested_ = true;
  {
    std::lock_guard<std::mutex> lk(cv_mtx_);
    resume_signaled_ = true;
    pending_step_ = pending_step{};
    // If we're detaching while sitting right on a just-hit breakpoint's
    // restored original instruction, do NOT ask wait_for_continue_command()
    // to arm the trap flag for the usual repatch-after-one-step dance: the
    // debug loop is about to break out on stop_requested_ without ever
    // processing that resulting single-step event, so the debuggee would
    // resume with its trap flag set and no debugger left to field the
    // exception -- it dies with an unhandled STATUS_SINGLE_STEP instead of
    // exiting normally. Detaching removes the breakpoint's effect anyway
    // (the INT3 is already gone from memory at this point), so there is
    // nothing left to repatch.
    awaiting_repatch_after_bp_ = false;
  }
  cv_.notify_all();
  debug_thread_.join();
}

void win32_debug_backend::pause() {
  if (!process_)
    return;
  pause_requested_ = true;
  DebugBreakProcess(process_);
}

void win32_debug_backend::resume() {
  {
    std::lock_guard<std::mutex> lk(cv_mtx_);
    pending_step_ = pending_step{};
    resume_signaled_ = true;
  }
  cv_.notify_all();
}

void win32_debug_backend::step_into(uint32_t /*thread_id*/) {
  {
    std::lock_guard<std::mutex> lk(cv_mtx_);
    pending_step_ = pending_step{};
    pending_step_.kind = pending_step::kind::into;
    resume_signaled_ = true;
  }
  cv_.notify_all();
}

void win32_debug_backend::step_over(uint32_t thread_id) {
  HANDLE th = thread_handle(thread_id);
  if (!th || !process_) {
    step_into(thread_id);
    return;
  }
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_FULL;
  GetThreadContext(th, &ctx);

  int len = decode_call_length(ctx_pc(ctx));
  if (len <= 0) {
    step_into(thread_id);
    return;
  }
  DWORD64 ret_addr = ctx_pc(ctx) + static_cast<DWORD64>(len);

  BYTE orig = 0;
  SIZE_T n = 0;
  if (!ReadProcessMemory(process_, reinterpret_cast<LPCVOID>(ret_addr), &orig, 1, &n) || n != 1) {
    step_into(thread_id);
    return;
  }
  BYTE int3 = 0xCC;
  WriteProcessMemory(process_, reinterpret_cast<LPVOID>(ret_addr), &int3, 1, nullptr);
  FlushInstructionCache(process_, reinterpret_cast<LPCVOID>(ret_addr), 1);

  {
    std::lock_guard<std::mutex> lk(cv_mtx_);
    pending_step_ = pending_step{};
    pending_step_.kind = pending_step::kind::over;
    pending_step_.target_address = ret_addr;
    pending_step_.temp_bp_original_byte = orig;
    pending_step_.temp_bp_patched = true;
    resume_signaled_ = true;
  }
  cv_.notify_all();
}

void win32_debug_backend::step_out(uint32_t thread_id) {
  HANDLE th = thread_handle(thread_id);
  if (!th || !process_) {
    step_into(thread_id);
    return;
  }
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_FULL;
  GetThreadContext(th, &ctx);

  STACKFRAME64 frame{};
  frame.AddrPC.Offset = ctx_pc(ctx);
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = ctx_fp(ctx);
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = ctx_sp(ctx);
  frame.AddrStack.Mode = AddrModeFlat;
  CONTEXT walk_ctx = ctx;

  DWORD64 ret_addr = 0;
  for (int i = 0; i < 2; ++i) {
    if (!StackWalk64(kMachineType, process_, th, &frame, &walk_ctx, nullptr, SymFunctionTableAccess64,
                      SymGetModuleBase64, nullptr))
      break;
    if (i == 1) {
      ret_addr = frame.AddrPC.Offset;
      break;
    }
  }
  if (!ret_addr) {
    step_into(thread_id);
    return;
  }

  BYTE orig = 0;
  SIZE_T n = 0;
  if (!ReadProcessMemory(process_, reinterpret_cast<LPCVOID>(ret_addr), &orig, 1, &n) || n != 1) {
    step_into(thread_id);
    return;
  }
  BYTE int3 = 0xCC;
  WriteProcessMemory(process_, reinterpret_cast<LPVOID>(ret_addr), &int3, 1, nullptr);
  FlushInstructionCache(process_, reinterpret_cast<LPCVOID>(ret_addr), 1);

  {
    std::lock_guard<std::mutex> lk(cv_mtx_);
    pending_step_ = pending_step{};
    pending_step_.kind = pending_step::kind::out;
    pending_step_.target_address = ret_addr;
    pending_step_.temp_bp_original_byte = orig;
    pending_step_.temp_bp_patched = true;
    resume_signaled_ = true;
  }
  cv_.notify_all();
}

bool win32_debug_backend::set_breakpoint(const std::string& file, int line) {
  if (!process_ || !sym_initialized_)
    return false;

  line_search_state st{&file, line};
  // Pass the known module base explicitly rather than 0 ("all modules") --
  // SymEnumLines needs a specific module to search, and with symbols loaded
  // eagerly (see handle_create_process) this is the only one that matters.
  SymEnumLines(process_, module_base_, nullptr, nullptr, enum_lines_cb, &st);
  if (st.best_addr == 0)
    return false;

  BYTE orig = 0;
  SIZE_T n = 0;
  if (!ReadProcessMemory(process_, reinterpret_cast<LPCVOID>(st.best_addr), &orig, 1, &n) || n != 1)
    return false;
  BYTE int3 = 0xCC;
  if (!WriteProcessMemory(process_, reinterpret_cast<LPVOID>(st.best_addr), &int3, 1, nullptr))
    return false;
  FlushInstructionCache(process_, reinterpret_cast<LPCVOID>(st.best_addr), 1);

  breakpoint_state bp;
  bp.file = file;
  bp.line = line;
  bp.address = st.best_addr;
  bp.original_byte = orig;
  bp.patched = true;
  breakpoints_.wlock()->push_back(bp);
  return true;
}

void win32_debug_backend::clear_breakpoint(const std::string& file, int line) {
  auto wl = breakpoints_.wlock();
  for (auto it = wl->begin(); it != wl->end(); ++it) {
    if (it->file != file || it->line != line)
      continue;
    if (it->patched && process_) {
      WriteProcessMemory(process_, reinterpret_cast<LPVOID>(it->address), &it->original_byte, 1, nullptr);
      FlushInstructionCache(process_, reinterpret_cast<LPCVOID>(it->address), 1);
    }
    wl->erase(it);
    return;
  }
}

std::vector<thread_info> win32_debug_backend::get_threads() {
  std::vector<thread_info> result;
  auto rl = thread_handles_.rlock();
  for (auto& [tid, handle] : *rl) {
    thread_info info;
    info.id = tid;
    info.state = "suspended";
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(handle, &ctx)) {
      std::string file;
      int32_t line = 0;
      resolve_address(ctx_pc(ctx), info.current_function, file, line);
    }
    result.push_back(std::move(info));
  }
  return result;
}

std::vector<frame_info> win32_debug_backend::get_callstack(uint32_t thread_id) {
  std::vector<frame_info> frames;
  HANDLE th = thread_handle(thread_id);
  if (!th || !process_)
    return frames;
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_FULL;
  if (!GetThreadContext(th, &ctx))
    return frames;

  STACKFRAME64 frame{};
  frame.AddrPC.Offset = ctx_pc(ctx);
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = ctx_fp(ctx);
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = ctx_sp(ctx);
  frame.AddrStack.Mode = AddrModeFlat;
  CONTEXT walk_ctx = ctx;

  for (int32_t i = 0; i < 64; ++i) {
    if (!StackWalk64(kMachineType, process_, th, &frame, &walk_ctx, nullptr, SymFunctionTableAccess64,
                      SymGetModuleBase64, nullptr))
      break;
    if (frame.AddrPC.Offset == 0)
      break;
    frame_info fi;
    fi.index = i;
    resolve_address(frame.AddrPC.Offset, fi.function, fi.file, fi.line);
    frames.push_back(std::move(fi));
  }
  return frames;
}

std::vector<watch_entry> win32_debug_backend::evaluate(uint32_t frame_id, const std::vector<std::string>& exprs) {
  // v1 scope (DESIGN.md §1): simple local-variable reads resolved via
  // DbgHelp's symbol enumerator, not arbitrary C++ expressions -- `exprs`
  // are matched verbatim against local names in scope at `frame_id`.
  std::vector<watch_entry> result;
  if (!sym_initialized_ || !process_ || exprs.empty())
    return result;
  HANDLE th = thread_handle(last_event_tid_);
  if (!th)
    return result;

  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_FULL;
  if (!GetThreadContext(th, &ctx))
    return result;

  STACKFRAME64 frame{};
  frame.AddrPC.Offset = ctx_pc(ctx);
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = ctx_fp(ctx);
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = ctx_sp(ctx);
  frame.AddrStack.Mode = AddrModeFlat;
  CONTEXT walk_ctx = ctx;

  // Walk to the requested frame (frame_id 0 == innermost, matching
  // get_callstack()'s indexing) so locals are resolved against the right
  // scope/frame pointer, not always the top of the stack.
  for (uint32_t i = 0; i <= frame_id; ++i) {
    if (!StackWalk64(kMachineType, process_, th, &frame, &walk_ctx, nullptr, SymFunctionTableAccess64,
                      SymGetModuleBase64, nullptr) ||
        frame.AddrPC.Offset == 0)
      return result; // frame_id deeper than the actual stack.
  }

  if (!SymSetScopeFromAddr(process_, frame.AddrPC.Offset))
    return result; // No local scope at this address (e.g. no debug info).

  watch_eval_state st{&exprs, process_, frame.AddrFrame.Offset, frame.AddrStack.Offset, &result};
  SymEnumSymbols(process_, 0, nullptr, &enum_locals_cb, &st);
  return result;
}

void win32_debug_backend::on_stop(stop_callback cb) {
  *on_stop_cb_.wlock() = std::move(cb);
}

void win32_debug_backend::on_log(log_callback cb) {
  *on_log_cb_.wlock() = std::move(cb);
}

// ── Debug thread ─────────────────────────────────────────────────────────

void win32_debug_backend::debug_thread_main(uint32_t pid) {
  DEBUG_EVENT dbg_event{};
  bool reported_attach = false;

  while (attached_) {
    if (!WaitForDebugEvent(&dbg_event, 200)) {
      if (stop_requested_)
        break;
      continue;
    }

    DWORD continue_status = DBG_CONTINUE;
    bool should_stop = false;
    stop_event evt;

    switch (dbg_event.dwDebugEventCode) {
      case CREATE_PROCESS_DEBUG_EVENT:
        handle_create_process(dbg_event);
        if (!reported_attach) {
          reported_attach = true;
          evt.thread_id = dbg_event.dwThreadId;
          evt.reason = "attach";
          std::string fn;
          resolve_address(reinterpret_cast<DWORD64>(dbg_event.u.CreateProcessInfo.lpStartAddress), fn, evt.file,
                           evt.line);
          should_stop = true;
        }
        if (dbg_event.u.CreateProcessInfo.hFile)
          CloseHandle(dbg_event.u.CreateProcessInfo.hFile);
        break;
      case LOAD_DLL_DEBUG_EVENT:
        handle_load_dll(dbg_event);
        if (dbg_event.u.LoadDll.hFile)
          CloseHandle(dbg_event.u.LoadDll.hFile);
        break;
      case CREATE_THREAD_DEBUG_EVENT:
        thread_handles_.wlock()->emplace(dbg_event.dwThreadId, dbg_event.u.CreateThread.hThread);
        break;
      case EXIT_THREAD_DEBUG_EVENT:
        handle_exit_thread(dbg_event);
        break;
      case EXCEPTION_DEBUG_EVENT:
        continue_status = handle_exception(dbg_event, should_stop, evt);
        break;
      case OUTPUT_DEBUG_STRING_EVENT:
        handle_output_debug_string(dbg_event);
        break;
      case EXIT_PROCESS_DEBUG_EVENT:
        attached_ = false;
        break;
      default:
        break;
    }

    if (should_stop) {
      last_event_tid_ = dbg_event.dwThreadId;
      stop_callback cb = *on_stop_cb_.rlock();
      if (cb)
        cb(evt);
      wait_for_continue_command();
    }

    ContinueDebugEvent(dbg_event.dwProcessId, dbg_event.dwThreadId, continue_status);

    if (stop_requested_ || dbg_event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT)
      break;
  }

  restore_all_breakpoints();
  if (pid)
    DebugActiveProcessStop(pid);
  attached_ = false;
}

void win32_debug_backend::handle_create_process(const DEBUG_EVENT& ev) {
  process_ = ev.u.CreateProcessInfo.hProcess;
  thread_handles_.wlock()->emplace(ev.dwThreadId, ev.u.CreateProcessInfo.hThread);

  // No SYMOPT_DEFERRED_LOADS: line-table lookups (set_breakpoint's
  // SymEnumLines) need the PDB's line data actually loaded, and letting it
  // load lazily on an address-based query alone proved unreliable in
  // practice. Eager loading costs a bit of attach latency but keeps
  // resolve_address()/set_breakpoint() simple and correct.
  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
  if (SymInitialize(process_, nullptr, FALSE)) {
    sym_initialized_ = true;
    module_base_ = SymLoadModuleEx(process_, ev.u.CreateProcessInfo.hFile, nullptr, nullptr,
                                    reinterpret_cast<DWORD64>(ev.u.CreateProcessInfo.lpBaseOfImage), 0, nullptr, 0);
  }
}

void win32_debug_backend::handle_load_dll(const DEBUG_EVENT& ev) {
  if (!sym_initialized_)
    return;
  SymLoadModuleEx(process_, ev.u.LoadDll.hFile, nullptr, nullptr,
                   reinterpret_cast<DWORD64>(ev.u.LoadDll.lpBaseOfDll), 0, nullptr, 0);
}

void win32_debug_backend::handle_exit_thread(const DEBUG_EVENT& ev) {
  thread_handles_.wlock()->erase(ev.dwThreadId);
}

void win32_debug_backend::handle_output_debug_string(const DEBUG_EVENT& ev) {
  log_callback cb = *on_log_cb_.rlock();
  if (!cb || !process_)
    return;
  const auto& info = ev.u.DebugString;
  if (!info.lpDebugStringData || info.nDebugStringLength == 0)
    return;

  std::string text;
  if (info.fUnicode) {
    std::vector<WCHAR> wbuf(info.nDebugStringLength);
    SIZE_T read = 0;
    if (!ReadProcessMemory(process_, info.lpDebugStringData, wbuf.data(), wbuf.size() * sizeof(WCHAR), &read) ||
        read == 0)
      return;
    int len = WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), static_cast<int>(read / sizeof(WCHAR)), nullptr, 0,
                                   nullptr, nullptr);
    if (len <= 0)
      return;
    text.resize(static_cast<size_t>(len));
    WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), static_cast<int>(read / sizeof(WCHAR)), text.data(), len, nullptr,
                         nullptr);
  } else {
    std::vector<char> buf(info.nDebugStringLength);
    SIZE_T read = 0;
    if (!ReadProcessMemory(process_, info.lpDebugStringData, buf.data(), buf.size(), &read) || read == 0)
      return;
    text.assign(buf.data(), read);
  }

  // Debug strings are NUL-terminated and commonly end in a newline (e.g.
  // OutputDebugPrintf-style helpers) -- both would otherwise show up as a
  // trailing blank cell in the Output table.
  while (!text.empty() && (text.back() == '\0' || text.back() == '\n' || text.back() == '\r'))
    text.pop_back();
  if (text.empty())
    return;

  cb(text, "info");
}

DWORD win32_debug_backend::handle_exception(const DEBUG_EVENT& ev, bool& should_stop, stop_event& out) {
  should_stop = false;
  DWORD code = ev.u.Exception.ExceptionRecord.ExceptionCode;
  HANDLE th = thread_handle(ev.dwThreadId);
  if (!th)
    return DBG_EXCEPTION_NOT_HANDLED;

  if (code == EXCEPTION_SINGLE_STEP) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    GetThreadContext(th, &ctx);
    ctx.EFlags &= ~0x100u; // Clear the trap flag we set to get here.
    SetThreadContext(th, &ctx);
    DWORD64 pc = ctx_pc(ctx);

    if (awaiting_repatch_after_bp_) {
      awaiting_repatch_after_bp_ = false;
      auto wl = breakpoints_.wlock();
      for (auto& bp : *wl) {
        if (bp.address == repatch_addr_) {
          BYTE int3 = 0xCC;
          WriteProcessMemory(process_, reinterpret_cast<LPVOID>(bp.address), &int3, 1, nullptr);
          FlushInstructionCache(process_, reinterpret_cast<LPCVOID>(bp.address), 1);
          bp.patched = true;
          break;
        }
      }
      // The forced single-step past the just-hit breakpoint's restored
      // instruction also satisfies a step_into requested while sitting on
      // it -- report it now rather than silently continuing.
      if (pending_step_.kind == pending_step::kind::into) {
        pending_step_.kind = pending_step::kind::none;
        out.thread_id = ev.dwThreadId;
        out.reason = "step";
        std::string fn;
        resolve_address(pc, fn, out.file, out.line);
        should_stop = true;
      }
      return DBG_CONTINUE;
    }

    if (pending_step_.kind == pending_step::kind::into) {
      pending_step_.kind = pending_step::kind::none;
      out.thread_id = ev.dwThreadId;
      out.reason = "step";
      std::string fn;
      resolve_address(pc, fn, out.file, out.line);
      should_stop = true;
    }
    return DBG_CONTINUE;
  }

  if (code == EXCEPTION_BREAKPOINT) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    GetThreadContext(th, &ctx);
    DWORD64 hit_addr = ctx_pc(ctx) - 1;

    if ((pending_step_.kind == pending_step::kind::over || pending_step_.kind == pending_step::kind::out) &&
        hit_addr == pending_step_.target_address) {
      WriteProcessMemory(process_, reinterpret_cast<LPVOID>(hit_addr), &pending_step_.temp_bp_original_byte, 1,
                          nullptr);
      FlushInstructionCache(process_, reinterpret_cast<LPCVOID>(hit_addr), 1);
      set_ctx_pc(ctx, hit_addr);
      SetThreadContext(th, &ctx);
      pending_step_ = pending_step{};
      out.thread_id = ev.dwThreadId;
      out.reason = "step";
      std::string fn;
      resolve_address(hit_addr, fn, out.file, out.line);
      should_stop = true;
      return DBG_CONTINUE;
    }

    {
      auto wl = breakpoints_.wlock();
      for (auto& bp : *wl) {
        if (!bp.patched || bp.address != hit_addr)
          continue;
        WriteProcessMemory(process_, reinterpret_cast<LPVOID>(hit_addr), &bp.original_byte, 1, nullptr);
        FlushInstructionCache(process_, reinterpret_cast<LPCVOID>(hit_addr), 1);
        bp.patched = false;
        set_ctx_pc(ctx, hit_addr);
        SetThreadContext(th, &ctx);
        out.thread_id = ev.dwThreadId;
        out.reason = "breakpoint";
        out.file = bp.file;
        out.line = bp.line;
        should_stop = true;
        awaiting_repatch_after_bp_ = true;
        repatch_addr_ = hit_addr;
        return DBG_CONTINUE;
      }
    }

    if (pause_requested_) {
      pause_requested_ = false;
      out.thread_id = ev.dwThreadId;
      out.reason = "pause";
      std::string fn;
      resolve_address(ctx_pc(ctx), fn, out.file, out.line);
      should_stop = true;
      return DBG_CONTINUE;
    }

    return DBG_EXCEPTION_NOT_HANDLED;
  }

  // Any other exception (access violation, integer divide-by-zero, a
  // debuggee-thrown C++ exception's first chance, ...): report it to the
  // Output window and pause like a breakpoint hit (DESIGN.md §4's
  // "exceptions" bullet), instead of silently forwarding it to whatever
  // handler the debuggee has. Only on the first chance -- a second-chance
  // exception has already run every SEH handler and is about to be fatal,
  // so there is nothing left to inspect and it is forwarded unhandled so
  // the process actually terminates instead of hanging on a stop no one
  // asked for.
  if (!ev.u.Exception.dwFirstChance)
    return DBG_EXCEPTION_NOT_HANDLED;

  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_FULL;
  GetThreadContext(th, &ctx);
  out.thread_id = ev.dwThreadId;
  out.reason = "exception";
  std::string fn;
  resolve_address(ctx_pc(ctx), fn, out.file, out.line);
  should_stop = true;
  return DBG_EXCEPTION_NOT_HANDLED;
}

bool win32_debug_backend::resolve_address(DWORD64 address, std::string& function, std::string& file, int32_t& line) {
  file.clear();
  line = 0;
  if (!sym_initialized_ || !process_)
    return false;

  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
  PSYMBOL_INFO sym = reinterpret_cast<PSYMBOL_INFO>(buffer);
  sym->SizeOfStruct = sizeof(SYMBOL_INFO);
  sym->MaxNameLen = MAX_SYM_NAME;
  DWORD64 displacement = 0;
  if (SymFromAddr(process_, address, &displacement, sym))
    function.assign(sym->Name, sym->NameLen);

  IMAGEHLP_LINE64 line_info{};
  line_info.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
  DWORD disp32 = 0;
  if (SymGetLineFromAddr64(process_, address, &disp32, &line_info)) {
    file = line_info.FileName ? line_info.FileName : "";
    line = static_cast<int32_t>(line_info.LineNumber);
    return true;
  }
  return false;
}

void win32_debug_backend::wait_for_continue_command() {
  std::unique_lock<std::mutex> lk(cv_mtx_);
  cv_.wait(lk, [&] { return resume_signaled_; });
  resume_signaled_ = false;
  bool want_trap = awaiting_repatch_after_bp_ || pending_step_.kind == pending_step::kind::into;
  lk.unlock();

  HANDLE th = thread_handle(last_event_tid_);
  if (!th)
    return;
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_FULL;
  GetThreadContext(th, &ctx);
  if (want_trap)
    ctx.EFlags |= 0x100u;
  else
    ctx.EFlags &= ~0x100u;
  SetThreadContext(th, &ctx);
}

int win32_debug_backend::decode_call_length(DWORD64 address) {
  BYTE buf[16]{};
  SIZE_T read = 0;
  if (!process_ || !ReadProcessMemory(process_, reinterpret_cast<LPCVOID>(address), buf, sizeof(buf), &read) ||
      read < 2)
    return 0;

  size_t i = 0;
  bool has_rex = (buf[i] & 0xF0) == 0x40;
  size_t rex_len = has_rex ? 1 : 0;

  if (buf[i + rex_len] == 0xE8)
    return static_cast<int>(rex_len) + 5; // CALL rel32

  if (buf[i + rex_len] == 0xFF) {
    BYTE modrm = buf[i + rex_len + 1];
    BYTE reg = (modrm >> 3) & 0x7;
    if (reg != 2)
      return 0; // FF /2 is CALL r/m; any other /reg isn't a call.
    BYTE mod = (modrm >> 6) & 0x3;
    BYTE rm = modrm & 0x7;
    size_t len = rex_len + 2; // opcode + ModRM
    bool has_sib = (mod != 3 && rm == 4);
    if (has_sib)
      len += 1;
    if (mod == 1)
      len += 1;
    else if (mod == 2)
      len += 4;
    else if (mod == 0 && rm == 5)
      len += 4; // RIP-relative disp32
    return static_cast<int>(len);
  }
  return 0;
}

void win32_debug_backend::restore_all_breakpoints() {
  auto wl = breakpoints_.wlock();
  for (auto& bp : *wl) {
    if (bp.patched && process_) {
      WriteProcessMemory(process_, reinterpret_cast<LPVOID>(bp.address), &bp.original_byte, 1, nullptr);
      FlushInstructionCache(process_, reinterpret_cast<LPCVOID>(bp.address), 1);
      bp.patched = false;
    }
  }
}

HANDLE win32_debug_backend::thread_handle(uint32_t thread_id) {
  auto rl = thread_handles_.rlock();
  auto it = rl->find(thread_id);
  return it == rl->end() ? nullptr : it->second;
}

} // namespace bdg::wish::dbg

#endif // defined(_WIN32)
