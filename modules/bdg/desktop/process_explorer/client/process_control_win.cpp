// MIT License © 2025 Binary Dice Games
/// @file process_control_win.cpp
/// @brief Windows implementation of process_control.hpp.
///
/// Implements the same interface as process_control_linux.cpp but with
/// documented Win32 APIs only (TerminateProcess, SuspendThread/ResumeThread
/// over a Toolhelp32Snapshot thread list -- Windows has no public
/// "suspend/resume whole process" call, so this loops threads the same way
/// Process Explorer/Task Manager do -- SetPriorityClass,
/// SetProcessAffinityMask/GetProcessAffinityMask). `process_details::cwd`
/// is always left empty here: reading another process's working directory
/// requires undocumented NtQueryInformationProcess/PEB access, which this
/// file intentionally avoids (see process_info_win.cpp's own doc comment on
/// sticking to documented APIs).
#include "process_control.hpp"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <sddl.h>

#include <string>
#include <vector>

namespace bdg::wish {

namespace {

process_action_result win32_error_result() {
  process_action_result r;
  r.success = false;
  DWORD err = GetLastError();
  LPSTR msg = nullptr;
  DWORD len = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr,
      err,
      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPSTR>(&msg),
      0,
      nullptr);
  if (len > 0 && msg) {
    r.error.assign(msg, len);
    while (!r.error.empty() && (r.error.back() == '\n' || r.error.back() == '\r'))
      r.error.pop_back();
  } else {
    r.error = "Win32 error " + std::to_string(err);
  }
  if (msg)
    LocalFree(msg);
  return r;
}

// Same discrete nice-value scale used server-side (process_explorer.cpp's
// kPriorityLevels) and mirrored by process_info_win.cpp's
// priority_class_to_nice() for the reverse direction -- kept in sync by
// hand since each direction lives in a different file/module.
DWORD nice_to_priority_class(int nice_value) {
  if (nice_value >= 15)
    return IDLE_PRIORITY_CLASS;
  if (nice_value >= 5)
    return BELOW_NORMAL_PRIORITY_CLASS;
  if (nice_value > -5)
    return NORMAL_PRIORITY_CLASS;
  if (nice_value > -15)
    return ABOVE_NORMAL_PRIORITY_CLASS;
  if (nice_value > -20)
    return HIGH_PRIORITY_CLASS;
  return REALTIME_PRIORITY_CLASS;
}

int priority_class_to_nice(DWORD priority_class) {
  switch (priority_class) {
    case IDLE_PRIORITY_CLASS:
      return 19;
    case BELOW_NORMAL_PRIORITY_CLASS:
      return 10;
    case ABOVE_NORMAL_PRIORITY_CLASS:
      return -5;
    case HIGH_PRIORITY_CLASS:
      return -10;
    case REALTIME_PRIORITY_CLASS:
      return -20;
    case NORMAL_PRIORITY_CLASS:
    default:
      return 0;
  }
}

// Applies `fn` (SuspendThread or ResumeThread) to every thread owned by
// `pid` -- the standard public-API technique for "suspend/resume a whole
// process" on Windows, since no single call does it directly.
process_action_result for_each_thread(int pid, DWORD (WINAPI* fn)(HANDLE)) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE)
    return win32_error_result();

  bool any = false;
  bool any_failed = false;
  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  if (Thread32First(snap, &te)) {
    do {
      if (te.th32OwnerProcessID != static_cast<DWORD>(pid))
        continue;
      HANDLE th = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
      if (th == NULL) {
        any_failed = true;
        continue;
      }
      any = true;
      if (fn(th) == static_cast<DWORD>(-1))
        any_failed = true;
      CloseHandle(th);
    } while (Thread32Next(snap, &te));
  }
  CloseHandle(snap);

  process_action_result r;
  if (!any) {
    r.success = false;
    r.error = "No such process";
  } else if (any_failed) {
    r.success = false;
    r.error = "Failed to suspend/resume one or more threads";
  }
  return r;
}

std::vector<int> get_process_affinity_cores(HANDLE h) {
  std::vector<int> cores;
  DWORD_PTR process_mask = 0, system_mask = 0;
  if (!GetProcessAffinityMask(h, &process_mask, &system_mask))
    return cores;
  for (int i = 0; i < static_cast<int>(sizeof(DWORD_PTR) * 8); ++i)
    if (process_mask & (static_cast<DWORD_PTR>(1) << i))
      cores.push_back(i);
  return cores;
}

std::string query_exe_path(HANDLE h) {
  wchar_t buf[MAX_PATH];
  DWORD len = MAX_PATH;
  if (!QueryFullProcessImageNameW(h, 0, buf, &len))
    return {};
  int needed = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
  if (needed <= 0)
    return {};
  std::string out(needed, '\0');
  WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(len), out.data(), needed, nullptr, nullptr);
  return out;
}

// Real (not effective) owning user name, via the process token's TokenUser SID.
std::string query_owner_name(HANDLE h) {
  HANDLE token = NULL;
  if (!OpenProcessToken(h, TOKEN_QUERY, &token))
    return {};

  DWORD needed = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
  std::string result;
  if (needed > 0) {
    std::vector<BYTE> buf(needed);
    if (GetTokenInformation(token, TokenUser, buf.data(), needed, &needed)) {
      auto* user = reinterpret_cast<TOKEN_USER*>(buf.data());
      wchar_t name[256], domain[256];
      DWORD name_len = 256, domain_len = 256;
      SID_NAME_USE use;
      if (LookupAccountSidW(nullptr, user->User.Sid, name, &name_len, domain, &domain_len, &use)) {
        int needed_bytes = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
        if (needed_bytes > 0) {
          std::string narrow(needed_bytes - 1, '\0');
          WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow.data(), needed_bytes, nullptr, nullptr);
          result = std::move(narrow);
        }
      }
    }
  }
  CloseHandle(token);
  return result;
}

std::string format_local_time(const FILETIME& creation_ft) {
  FILETIME local_ft{};
  SYSTEMTIME st{};
  if (!FileTimeToLocalFileTime(&creation_ft, &local_ft) || !FileTimeToSystemTime(&local_ft, &st))
    return {};
  char buf[64];
  std::snprintf(
      buf,
      sizeof(buf),
      "%04u-%02u-%02u %02u:%02u:%02u",
      st.wYear,
      st.wMonth,
      st.wDay,
      st.wHour,
      st.wMinute,
      st.wSecond);
  return buf;
}

} // namespace

process_action_result kill_process(int pid) {
  process_action_result r;
  HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
  if (h == NULL)
    return win32_error_result();
  if (!TerminateProcess(h, 1))
    r = win32_error_result();
  CloseHandle(h);
  return r;
}

process_action_result pause_process(int pid) {
  return for_each_thread(pid, SuspendThread);
}

process_action_result resume_process(int pid) {
  return for_each_thread(pid, ResumeThread);
}

process_action_result set_process_priority(int pid, int nice_value) {
  process_action_result r;
  HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (h == NULL)
    return win32_error_result();
  if (!SetPriorityClass(h, nice_to_priority_class(nice_value)))
    r = win32_error_result();
  CloseHandle(h);
  return r;
}

process_action_result set_process_affinity(int pid, const std::vector<int>& cores) {
  process_action_result r;
  if (cores.empty()) {
    r.success = false;
    r.error = "affinity core list must not be empty";
    return r;
  }
  HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (h == NULL)
    return win32_error_result();

  DWORD_PTR mask = 0;
  for (int core : cores)
    if (core >= 0 && core < static_cast<int>(sizeof(DWORD_PTR) * 8))
      mask |= (static_cast<DWORD_PTR>(1) << core);

  if (!SetProcessAffinityMask(h, mask))
    r = win32_error_result();
  CloseHandle(h);
  return r;
}

process_details get_process_details(int pid) {
  process_details d;
  d.pid = pid;

  HANDLE h = OpenProcess(
      PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
      FALSE,
      static_cast<DWORD>(pid));
  if (h == NULL) {
    d.error = "No such process (or access denied)";
    return d;
  }

  FILETIME creation, exit, kernel, user;
  if (GetProcessTimes(h, &creation, &exit, &kernel, &user))
    d.start_time = format_local_time(creation);

  d.nice = priority_class_to_nice(GetPriorityClass(h));
  d.affinity_cores = get_process_affinity_cores(h);
  d.exe_path = query_exe_path(h);
  d.user = query_owner_name(h);
  d.cmdline = d.exe_path.empty() ? std::string{} : d.exe_path;
  // cwd intentionally left empty -- see file header comment.

  CloseHandle(h);

  // ppid and thread count come from a process snapshot, not a process
  // HANDLE -- GetProcessTimes()/etc. above already confirmed the process
  // exists, so a miss here just leaves these fields at their defaults.
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap != INVALID_HANDLE_VALUE) {
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
      do {
        if (pe.th32ProcessID == static_cast<DWORD>(pid)) {
          d.ppid = static_cast<int>(pe.th32ParentProcessID);
          d.thread_count = static_cast<int>(pe.cntThreads);
          if (d.cmdline.empty()) {
            int needed = WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, nullptr, 0, nullptr, nullptr);
            if (needed > 0) {
              std::string name(needed - 1, '\0');
              WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, name.data(), needed, nullptr, nullptr);
              d.cmdline = "[" + name + "]";
            }
          }
          break;
        }
      } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
  }

  d.found = true;
  return d;
}

} // namespace bdg::wish
