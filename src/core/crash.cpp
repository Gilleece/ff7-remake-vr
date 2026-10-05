#include "ff7vr/core/crash.h"

#include "ff7vr/core/log.h"
#include "ff7vr/core/module.h"

#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <format>
#include <string>

namespace ff7vr::crash {
namespace {

constexpr int kMaxFrames = 32;

Options g_opts;
std::atomic<bool> g_installed{false};
PVOID g_veh = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter = nullptr;

// Hand-off between the faulting thread and the reporter thread.
HANDLE g_worker = nullptr;
HANDLE g_request = nullptr;    // auto-reset: faulting thread -> worker
HANDLE g_done = nullptr;       // auto-reset: worker -> faulting thread
SRWLOCK g_serial = SRWLOCK_INIT;  // one crash report at a time
EXCEPTION_POINTERS* g_ep = nullptr;
DWORD g_fault_tid = 0;
bool g_first_chance = false;
const char* g_manual_reason = nullptr;
std::filesystem::path g_last_dump;

std::atomic<int> g_reports{0};
std::atomic<int> g_dumps{0};
std::atomic<int> g_dump_seq{0};

bool is_fatal_code(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        case 0xC0000374:  // STATUS_HEAP_CORRUPTION
        case 0xC0000409:  // STATUS_STACK_BUFFER_OVERRUN / fail fast
            return true;
        default:
            return false;
    }
}

const char* code_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
        case EXCEPTION_BREAKPOINT: return "BREAKPOINT";
        case 0xC0000374: return "HEAP_CORRUPTION";
        case 0xC0000409: return "STACK_BUFFER_OVERRUN";
        case 0xE06D7363: return "C++ exception";
        default: return "unknown";
    }
}

// Walks the stack from a CONTEXT using the x64 unwind tables. No C++ objects
// with destructors here so __try is allowed. Returns the number of frames.
int walk_stack(const CONTEXT* start, DWORD64* frames, int max_frames) {
    CONTEXT ctx = *start;
    int n = 0;
    __try {
        while (n < max_frames && ctx.Rip) {
            frames[n++] = ctx.Rip;
            DWORD64 image_base = 0;
            PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
            if (!rf) {
                // Leaf function, or a jump to garbage: return address is at [rsp].
                ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                ctx.Rsp += 8;
            } else {
                PVOID handler_data = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, rf, &ctx, &handler_data, &establisher,
                                 nullptr);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

std::filesystem::path make_dump_path() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    int seq = g_dump_seq.fetch_add(1);
    std::wstring name = std::format(L"ff7vr-crash-{:04}{:02}{:02}-{:02}{:02}{:02}-{}-{}.dmp", t.wYear, t.wMonth,
                                    t.wDay, t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId(), seq);
    return g_opts.dump_dir / name;
}

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                           PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

MiniDumpWriteDumpFn get_minidump_fn() {
    static MiniDumpWriteDumpFn fn = [] {
        HMODULE h = GetModuleHandleW(L"dbghelp.dll");
        if (!h) {
            wchar_t sys[MAX_PATH];
            GetSystemDirectoryW(sys, MAX_PATH);
            h = LoadLibraryW((std::wstring(sys) + L"\\dbghelp.dll").c_str());
        }
        return h ? reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(h, "MiniDumpWriteDump")) : nullptr;
    }();
    return fn;
}

std::filesystem::path write_dump(EXCEPTION_POINTERS* ep, DWORD tid) {
    auto fn = get_minidump_fn();
    if (!fn) {
        log::write_crash("crash: dbghelp MiniDumpWriteDump unavailable");
        return {};
    }
    std::filesystem::path path = make_dump_path();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        log::write_crash(std::format("crash: cannot create dump file {} (error {})", log::narrow(path.wstring()),
                                     GetLastError()));
        return {};
    }
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = tid;
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    auto type = static_cast<MINIDUMP_TYPE>(
        g_opts.full_memory_dump
            ? (MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules)
            : (MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
               MiniDumpWithHandleData | MiniDumpScanMemory));
    BOOL ok = fn(GetCurrentProcess(), GetCurrentProcessId(), f, type, ep ? &mei : nullptr, nullptr, nullptr);
    DWORD err = ok ? 0 : GetLastError();
    CloseHandle(f);
    if (!ok) {
        log::write_crash(std::format("crash: MiniDumpWriteDump failed (0x{:x})", err));
        DeleteFileW(path.c_str());
        return {};
    }
    return path;
}

void report(EXCEPTION_POINTERS* ep, DWORD tid, bool first_chance) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const CONTEXT* c = ep->ContextRecord;
    const auto addr = reinterpret_cast<std::uintptr_t>(er->ExceptionAddress);
    int report_no = g_reports.fetch_add(1) + 1;

    std::string head = std::format("CRASH {}: exception 0x{:08X} {} at {} (thread {}, report {}/{})",
                                   first_chance ? "(first-chance)" : "(unhandled)", er->ExceptionCode,
                                   code_name(er->ExceptionCode), module::describe(addr), tid, report_no,
                                   g_opts.max_reports);
    log::write_crash(head);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || er->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) {
        if (er->NumberParameters >= 2) {
            const char* op = er->ExceptionInformation[0] == 0 ? "read" : er->ExceptionInformation[0] == 1 ? "write" : "execute";
            log::write_crash(std::format("  {} of address 0x{:x}", op, er->ExceptionInformation[1]));
        }
    }
    log::write_crash(std::format("  rip={:016x} rsp={:016x} rbp={:016x}", c->Rip, c->Rsp, c->Rbp));
    log::write_crash(std::format("  rax={:016x} rbx={:016x} rcx={:016x} rdx={:016x}", c->Rax, c->Rbx, c->Rcx, c->Rdx));
    log::write_crash(std::format("  rsi={:016x} rdi={:016x} r8 ={:016x} r9 ={:016x}", c->Rsi, c->Rdi, c->R8, c->R9));
    log::write_crash(std::format("  r10={:016x} r11={:016x} r12={:016x} r13={:016x}", c->R10, c->R11, c->R12, c->R13));
    log::write_crash(std::format("  r14={:016x} r15={:016x}", c->R14, c->R15));

    DWORD64 frames[kMaxFrames] = {};
    int n = walk_stack(c, frames, kMaxFrames);
    log::write_crash(std::format("  stack ({} frames):", n));
    for (int i = 0; i < n; ++i)
        log::write_crash(std::format("    #{:02} {}", i, module::describe(static_cast<std::uintptr_t>(frames[i]))));

    if (g_dumps.load() < g_opts.max_dumps) {
        g_dumps.fetch_add(1);
        auto path = write_dump(ep, tid);
        if (!path.empty()) log::write_crash(std::format("  minidump written: {}", log::narrow(path.wstring())));
    } else {
        log::write_crash("  (dump limit reached, no minidump)");
    }
}

DWORD WINAPI worker_main(void*) {
    for (;;) {
        if (WaitForSingleObject(g_request, INFINITE) != WAIT_OBJECT_0) return 0;
        if (g_manual_reason) {
            log::write_crash(std::format("manual dump requested: {}", g_manual_reason));
            g_last_dump = write_dump(nullptr, 0);
        } else if (g_ep) {
            report(g_ep, g_fault_tid, g_first_chance);
        }
        SetEvent(g_done);
    }
}

// Runs on the faulting thread (possibly with almost no stack left).
void hand_off(EXCEPTION_POINTERS* ep, bool first_chance) {
    if (GetCurrentThreadId() == GetThreadId(g_worker)) return;  // crash inside the reporter: give up
    if (g_reports.load() >= g_opts.max_reports) return;
    AcquireSRWLockExclusive(&g_serial);
    g_ep = ep;
    g_fault_tid = GetCurrentThreadId();
    g_first_chance = first_chance;
    g_manual_reason = nullptr;
    SetEvent(g_request);
    WaitForSingleObject(g_done, 60000);
    g_ep = nullptr;
    ReleaseSRWLockExclusive(&g_serial);
}

LONG CALLBACK vectored_handler(EXCEPTION_POINTERS* ep) {
    if (is_fatal_code(ep->ExceptionRecord->ExceptionCode)) hand_off(ep, true);
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* ep) {
    // Fatal codes were already reported first-chance; report the rest here.
    if (!g_opts.first_chance || !is_fatal_code(ep->ExceptionRecord->ExceptionCode)) hand_off(ep, false);
    else log::write_crash(std::format("CRASH (unhandled): exception 0x{:08X} reached the top-level filter (reported above)",
                                      ep->ExceptionRecord->ExceptionCode));
    return g_prev_filter ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

bool install(const Options& options) {
    if (g_installed.exchange(true)) return true;
    g_opts = options;
    if (g_opts.dump_dir.empty()) g_opts.dump_dir = module::self().path.parent_path();
    get_minidump_fn();  // resolve dbghelp now, not while crashing
    g_request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // Generous stack for formatting and MiniDumpWriteDump.
    g_worker = CreateThread(nullptr, 1024 * 1024, worker_main, nullptr, 0, nullptr);
    if (!g_request || !g_done || !g_worker) {
        log::error("crash: failed to create reporter thread/events ({})", GetLastError());
        return false;
    }
    SetThreadDescription(g_worker, L"ff7vr crash reporter");
    if (g_opts.first_chance) g_veh = AddVectoredExceptionHandler(1, vectored_handler);
    g_prev_filter = SetUnhandledExceptionFilter(unhandled_filter);
    log::info("crash: handler installed (first_chance={}, max_reports={}, max_dumps={}, dumps in {})",
              g_opts.first_chance, g_opts.max_reports, g_opts.max_dumps, log::narrow(g_opts.dump_dir.wstring()));
    return true;
}

void uninstall() {
    if (!g_installed.exchange(false)) return;
    if (g_veh) RemoveVectoredExceptionHandler(g_veh);
    g_veh = nullptr;
    SetUnhandledExceptionFilter(g_prev_filter);
}

std::filesystem::path write_dump_now(const char* reason) {
    if (!g_installed) return {};
    AcquireSRWLockExclusive(&g_serial);
    g_manual_reason = reason;
    g_ep = nullptr;
    SetEvent(g_request);
    WaitForSingleObject(g_done, 60000);
    g_manual_reason = nullptr;
    auto p = g_last_dump;
    ReleaseSRWLockExclusive(&g_serial);
    return p;
}

}  // namespace ff7vr::crash
