// Dev-only in-process inspection server (enabled by RE3C_dev.flag). Replaces Frida/Cheat Engine inside the
// 32-bit game, whose address space is too full for an injected agent once a level is loaded.
// TCP 127.0.0.1:47002, one text command per line, reply terminated by a line with a single '.'.
//   read <addr> <len>                hex dump
//   write <addr> <hex>
//   f32 <addr> <n>                   floats
//   watch <addr> <len> <w|rw|x> <ms> hardware breakpoint on all threads; unique EIPs + sample registers
//   scan32 <value-hex>               exact dword scan of writable memory (first pass)
//   scanf <value> <tol>              float scan (first pass)
//   next32 <value-hex> | nextf <value> <tol> | nextchanged | nextsame    filter previous results
//   list [n]                          print current results
//   aob <hex ?? pattern>             scan the exe image
#include "common.h"
#include <winsock2.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <string>

static bool DevEnabled() {
    char path[MAX_PATH];
    wsprintfA(path, "%sRE3C_dev.flag", ModDir());
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

// ---------------------------------------------------------------- hardware breakpoints
struct HitInfo {
    DWORD eip, eax, ebx, ecx, edx, esi, edi, ebp, esp, count;
    DWORD stack[6];
};
static volatile LONG g_watchActive = 0;
static volatile LONG g_watchByEcx = 0;  // dedup hits by (eip, ecx) instead of eip
static DWORD g_watchAddr = 0;
static HitInfo g_hits[64];
static volatile LONG g_hitCount = 0;
static CRITICAL_SECTION g_hitLock;
static PVOID g_veh = nullptr;

static DWORD g_vehStart = 0;
static volatile LONG g_crashLogs = 0;

// A crash in the game (access violation, a minute after start: the protection's own exceptions at startup are not
// logged): the registers and the code-like values on the stack, so the culprit can be found in the log.
static void LogCrash(EXCEPTION_POINTERS* ep) {
    const CONTEXT* c = ep->ContextRecord;
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    Log("dev: access violation at %08lx (%s %08lx) eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx "
        "ebp %08lx esp %08lx", c->Eip, r->NumberParameters > 0 && r->ExceptionInformation[0] ? "write" : "read",
        r->NumberParameters > 1 ? (DWORD)r->ExceptionInformation[1] : 0, c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi,
        c->Edi, c->Ebp, c->Esp);
    HMODULE mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&LogCrash, &mod);
    const DWORD modBase = (DWORD)(uintptr_t)mod;
    char line[640];
    int len = wsprintfA(line, "dev: stack (mod base %08lx):", modBase);
    int found = 0;
    for (int i = 0; i < 256 && found < 28; ++i) {
        const DWORD a = c->Esp + i * 4;
        if (!Readable((void*)a, 4)) break;
        const DWORD v = *(const DWORD*)a;
        if ((v >= 0x00401000 && v < 0x01000000) || (modBase && v >= modBase && v < modBase + 0x00200000)) {
            len += wsprintfA(line + len, " +%x:%08lx", i * 4, v);
            ++found;
        }
    }
    Log("%s", line);
}

static LONG CALLBACK Veh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && GetTickCount() - g_vehStart > 60000 &&
        InterlockedIncrement(&g_crashLogs) <= 3)
        LogCrash(ep);
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    if (!(c->Dr6 & 0x1)) return EXCEPTION_CONTINUE_SEARCH;  // only DR0 is ours
    if (!g_watchActive) {
        // A late trap after the watch was switched off: swallow it instead of letting the game see it.
        c->Dr6 = 0;
        c->Dr7 &= ~0xF0003u;
        c->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    EnterCriticalSection(&g_hitLock);
    LONG n = g_hitCount;
    int i = 0;
    for (; i < n; ++i)
        if (g_hits[i].eip == c->Eip && (!g_watchByEcx || g_hits[i].ecx == c->Ecx)) { g_hits[i].count++; break; }
    if (i == n && n < 64) {
        HitInfo& h = g_hits[n];
        h = HitInfo{c->Eip, c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, 1, {}};
        if (Readable((void*)c->Esp, 24)) memcpy(h.stack, (void*)c->Esp, 24);
        g_hitCount = n + 1;
    }
    LeaveCriticalSection(&g_hitLock);
    c->Dr6 = 0;
    c->EFlags |= 0x10000;  // RF: don't re-trigger an execute breakpoint on resume
    return EXCEPTION_CONTINUE_EXECUTION;
}

static int g_lastThreadsSet = 0, g_lastThreadsFailed = 0;

static void SetDebugRegsAllThreads(DWORD addr, int len, int type, bool enable) {
    g_lastThreadsSet = g_lastThreadsFailed = 0;
    DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = {sizeof(te)};
    int lenBits = len == 1 ? 0 : len == 2 ? 1 : 3;
    int rw = type;  // 0 exec, 1 write, 3 read/write
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self) continue;
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!th) continue;
        SuspendThread(th);
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(th, &c)) {
            if (enable) {
                c.Dr0 = addr;
                c.Dr7 = (c.Dr7 & ~0xF0003u) | 1u | ((DWORD)(rw | (lenBits << 2)) << 16);
            } else {
                c.Dr0 = 0;
                c.Dr7 &= ~0xF0003u;
            }
            if (SetThreadContext(th, &c)) g_lastThreadsSet++; else g_lastThreadsFailed++;
        } else {
            g_lastThreadsFailed++;
        }
        ResumeThread(th);
        CloseHandle(th);
    }
    CloseHandle(snap);
}

// ---------------------------------------------------------------- value scanner
static std::vector<DWORD> g_results;
static std::vector<DWORD> g_prevVals;

template <class F>
static void ForEachWritableRegion(F f) {
    MEMORY_BASIC_INFORMATION mbi;
    BYTE* p = (BYTE*)0x10000;
    while ((DWORD)p < 0x7FFE0000 && VirtualQuery(p, &mbi, sizeof(mbi))) {
        const DWORD w = PAGE_READWRITE | PAGE_EXECUTE_READWRITE;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & w) && !(mbi.Protect & PAGE_GUARD) &&
            (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_IMAGE))
            f((BYTE*)mbi.BaseAddress, mbi.RegionSize);
        p = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
    }
}

static std::string ScanFirst(bool isFloat, DWORD ival, float fval, float tol) {
    g_results.clear();
    g_prevVals.clear();
    ForEachWritableRegion([&](BYTE* base, SIZE_T size) {
        if ((DWORD)base >= 0x71000000 && (DWORD)base < 0x80000000) return;  // skip system dlls
        // Copy through ReadProcessMemory: the game may free a region while it is being scanned.
        static BYTE chunk[0x10000];
        for (SIZE_T c = 0; c < size; c += sizeof(chunk)) {
            const SIZE_T n = size - c < sizeof(chunk) ? size - c : sizeof(chunk);
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), base + c, chunk, n, &got)) continue;
            for (SIZE_T o = 0; o + 4 <= got; o += 4) {
                DWORD v = *(DWORD*)(chunk + o);
                bool hit = isFloat ? (fabsf(*(float*)&v - fval) <= tol) : (v == ival);
                if (hit && g_results.size() < 2000000) {
                    g_results.push_back((DWORD)(base + c + o));
                    g_prevVals.push_back(v);
                }
            }
        }
    });
    char buf[64];
    sprintf(buf, "%u results", (unsigned)g_results.size());
    return buf;
}

static std::string ScanNext(int mode, DWORD ival, float fval, float tol) {
    std::vector<DWORD> r, pv;
    for (size_t i = 0; i < g_results.size(); ++i) {
        DWORD a = g_results[i];
        DWORD v = 0;
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (void*)a, &v, 4, &got) || got != 4) continue;
        bool keep = false;
        switch (mode) {
            case 0: keep = v == ival; break;
            case 1: keep = fabsf(*(float*)&v - fval) <= tol; break;
            case 2: keep = v != g_prevVals[i]; break;
            case 3: keep = v == g_prevVals[i]; break;
        }
        if (keep) { r.push_back(a); pv.push_back(v); }
    }
    g_results.swap(r);
    g_prevVals.swap(pv);
    char buf[64];
    sprintf(buf, "%u results", (unsigned)g_results.size());
    return buf;
}

// ---------------------------------------------------------------- command handling
static std::string Hex(const BYTE* p, int n) {
    std::string s;
    char b[4];
    for (int i = 0; i < n; ++i) { sprintf(b, "%02x", p[i]); s += b; }
    return s;
}

static std::string Handle(char* line) {
    char* tok[8] = {};
    int nt = 0;
    for (char* t = strtok(line, " \r\n"); t && nt < 8; t = strtok(nullptr, " \r\n")) tok[nt++] = t;
    if (!nt) return "";
    std::string cmd = tok[0];
    char out[512];
    if (cmd == "read" && nt >= 3) {
        DWORD a = strtoul(tok[1], nullptr, 16), n = strtoul(tok[2], nullptr, 0);
        if (n > 0x10000) n = 0x10000;
        static BYTE rbuf[0x10000];
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (void*)a, rbuf, n, &got) || got != n) return "unreadable";
        return Hex(rbuf, (int)n);
    }
    if (cmd == "write" && nt >= 3) {
        DWORD a = strtoul(tok[1], nullptr, 16);
        size_t n = strlen(tok[2]) / 2;
        std::vector<BYTE> b(n);
        for (size_t i = 0; i < n; ++i) { char h[3] = {tok[2][2 * i], tok[2][2 * i + 1], 0}; b[i] = (BYTE)strtoul(h, nullptr, 16); }
        DWORD old;
        if (!VirtualProtect((void*)a, n, PAGE_EXECUTE_READWRITE, &old)) return "protect failed";
        memcpy((void*)a, b.data(), n);
        VirtualProtect((void*)a, n, old, &old);
        return "ok";
    }
    if (cmd == "f32" && nt >= 3) {
        DWORD a = strtoul(tok[1], nullptr, 16), n = strtoul(tok[2], nullptr, 0);
        if (n > 256) n = 256;
        if (!Readable((void*)a, n * 4)) return "unreadable";
        std::string s;
        for (DWORD i = 0; i < n; ++i) { sprintf(out, "%.5f ", ((float*)a)[i]); s += out; }
        return s;
    }
    if ((cmd == "watch" || cmd == "watchx") && nt >= 5) {
        g_watchByEcx = cmd == "watchx";
        DWORD a = strtoul(tok[1], nullptr, 16);
        int len = atoi(tok[2]);
        int type = !strcmp(tok[3], "x") ? 0 : !strcmp(tok[3], "w") ? 1 : 3;
        if (type == 0) len = 1;
        DWORD ms = strtoul(tok[4], nullptr, 0);
        g_hitCount = 0;
        g_watchAddr = a;
        g_watchActive = 1;
        SetDebugRegsAllThreads(a, len, type, true);
        Sleep(ms);
        SetDebugRegsAllThreads(a, len, type, false);
        Sleep(200);
        g_watchActive = 0;
        std::string s;
        EnterCriticalSection(&g_hitLock);
        for (LONG i = 0; i < g_hitCount; ++i) {
            HitInfo& h = g_hits[i];
            sprintf(out,
                    "eip=%08lx n=%lu eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx "
                    "stk=%08lx %08lx %08lx %08lx %08lx %08lx\n",
                    h.eip, h.count, h.eax, h.ebx, h.ecx, h.edx, h.esi, h.edi, h.ebp, h.esp, h.stack[0], h.stack[1],
                    h.stack[2], h.stack[3], h.stack[4], h.stack[5]);
            s += out;
        }
        LeaveCriticalSection(&g_hitLock);
        sprintf(out, "threads set=%d failed=%d\n", g_lastThreadsSet, g_lastThreadsFailed);
        return std::string(out) + (s.empty() ? "no hits" : s);
    }
    if (cmd == "scan32" && nt >= 2) return ScanFirst(false, strtoul(tok[1], nullptr, 16), 0, 0);
    if (cmd == "scanf" && nt >= 3) return ScanFirst(true, 0, (float)atof(tok[1]), (float)atof(tok[2]));
    if (cmd == "next32" && nt >= 2) return ScanNext(0, strtoul(tok[1], nullptr, 16), 0, 0);
    if (cmd == "nextf" && nt >= 3) return ScanNext(1, 0, (float)atof(tok[1]), (float)atof(tok[2]));
    if (cmd == "nextchanged") return ScanNext(2, 0, 0, 0);
    if (cmd == "nextsame") return ScanNext(3, 0, 0, 0);
    if (cmd == "list") {
        size_t n = nt >= 2 ? strtoul(tok[1], nullptr, 0) : 50;
        std::string s;
        for (size_t i = 0; i < g_results.size() && i < n; ++i) {
            DWORD a = g_results[i];
            DWORD v = Readable((void*)a, 4) ? *(DWORD*)a : 0;
            sprintf(out, "%08lx %08lx %.5f\n", a, v, *(float*)&v);
            s += out;
        }
        return s;
    }
    if (cmd == "aob" && nt >= 2) {
        // pattern tokens were split by spaces; rebuild from the rest of the tokens
        std::vector<int> pat;
        for (int i = 1; i < nt; ++i) {
            for (char* t = tok[i]; *t; t += 2) {
                if (t[0] == '?') pat.push_back(-1);
                else { char h[3] = {t[0], t[1], 0}; pat.push_back((int)strtoul(h, nullptr, 16)); }
                if (!t[1]) break;
            }
        }
        std::string s;
        BYTE* lo = (BYTE*)0x401000;
        BYTE* hi = (BYTE*)0xF4A000;
        int found = 0;
        for (BYTE* p = lo; p + pat.size() < hi && found < 50; ++p) {
            size_t k = 0;
            for (; k < pat.size(); ++k)
                if (pat[k] >= 0 && p[k] != (BYTE)pat[k]) break;
            if (k == pat.size()) { sprintf(out, "%08lx\n", (DWORD)p); s += out; ++found; }
        }
        return s.empty() ? "none" : s;
    }
    return "unknown command";
}

static DWORD WINAPI ServerThread(LPVOID) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(47002);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(srv, 2) != 0) {
        Log("dev: server bind failed %d", WSAGetLastError());
        return 0;
    }
    Log("dev: inspection server on 127.0.0.1:47002");
    for (;;) {
        SOCKET c = accept(srv, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        std::string buf;
        char tmp[4096];
        for (;;) {
            int r = recv(c, tmp, sizeof(tmp), 0);
            if (r <= 0) break;
            buf.append(tmp, r);
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                std::vector<char> lb(line.begin(), line.end());
                lb.push_back(0);
                std::string reply = Handle(lb.data()) + "\n.\n";
                send(c, reply.data(), (int)reply.size(), 0);
            }
        }
        closesocket(c);
    }
}

void InitDevServer() {
    if (!DevEnabled()) return;
    InitializeCriticalSection(&g_hitLock);
    g_vehStart = GetTickCount();
    g_veh = AddVectoredExceptionHandler(1, Veh);
    HANDLE t = CreateThread(nullptr, 0, ServerThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}
