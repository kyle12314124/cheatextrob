#include "memory.h"
#include "offsets.h"
#include <psapi.h>
#include <tlhelp32.h>
#include <cstring>
#include <cctype>
#include <algorithm>

#pragma comment(lib, "psapi.lib")

namespace mem {

Process g;

namespace {

uint32_t findPid(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    uint32_t pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// Parses one pattern token. Returns byte value + wildcard flag.
bool parseToken(const char* tok, uint8_t& value, bool& wildcard) {
    wildcard = false;
    if (tok[0] == '?' && tok[1] == '?') { wildcard = true; return true; }
    if (tok[0] == '?' ) { wildcard = true; return true; }
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int hi = hex(tok[0]);
    int lo = hex(tok[1]);
    if (hi < 0 || lo < 0) return false;
    value = static_cast<uint8_t>((hi << 4) | lo);
    return true;
}

} // namespace

bool Process::attach(const wchar_t* processName) {
    detach();
    pid_ = findPid(processName);
    if (!pid_) return false;

    handle_ = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION
                          | PROCESS_QUERY_INFORMATION, FALSE, pid_);
    if (!handle_) { pid_ = 0; return false; }

    base_ = moduleBase(processName);
    return base_ != 0;
}

void Process::detach() {
    if (handle_) CloseHandle(handle_);
    handle_ = nullptr;
    pid_ = 0;
    base_ = 0;
}

bool Process::readRaw(uintptr_t address, void* buffer, size_t size) const {
    if (!handle_ || !address || !size) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(handle_, reinterpret_cast<LPCVOID>(address),
                             buffer, size, &got) && got == size;
}

bool Process::writeRaw(uintptr_t address, const void* buffer, size_t size) const {
    if (!handle_ || !address || !size) return false;
    SIZE_T put = 0;
    return WriteProcessMemory(handle_, reinterpret_cast<LPVOID>(address),
                              buffer, size, &put) && put == size;
}

std::wstring Process::modulePath(const wchar_t* moduleName) const {
    if (!handle_) return {};
    HMODULE mods[1024]{};
    DWORD needed = 0;
    if (!EnumProcessModulesEx(handle_, mods, sizeof(mods), &needed, LIST_MODULES_ALL))
        return {};

    wchar_t name[MAX_PATH]{};
    wchar_t path[MAX_PATH]{};
    for (DWORD i = 0; i < needed / sizeof(HMODULE); ++i) {
        if (!GetModuleBaseNameW(handle_, mods[i], name, MAX_PATH)) continue;
        if (_wcsicmp(name, moduleName) != 0) continue;
        DWORD n = GetModuleFileNameExW(handle_, mods[i], path, MAX_PATH);
        if (!n) return {};
        return std::wstring(path, n);
    }
    return {};
}

uintptr_t Process::moduleBase(const wchar_t* moduleName) const {
    if (!handle_) return 0;
    HMODULE mods[1024]{};
    DWORD needed = 0;
    if (!EnumProcessModulesEx(handle_, mods, sizeof(mods), &needed, LIST_MODULES_ALL))
        return 0;

    wchar_t name[MAX_PATH]{};
    for (DWORD i = 0; i < needed / sizeof(HMODULE); ++i) {
        if (!GetModuleBaseNameW(handle_, mods[i], name, MAX_PATH)) continue;
        if (_wcsicmp(name, moduleName) == 0)
            return reinterpret_cast<uintptr_t>(mods[i]);
    }
    return 0;
}

size_t Process::moduleSize(const wchar_t* moduleName) const {
    uintptr_t base = moduleBase(moduleName);
    if (!base) return 0;
    MODULEINFO mi{};
    if (!GetModuleInformation(handle_, reinterpret_cast<HMODULE>(base), &mi, sizeof(mi)))
        return 0;
    return mi.SizeOfImage;
}

std::vector<Process::Range> Process::moduleWritableRanges(const wchar_t* moduleName) const {
    std::vector<Range> out;
    uintptr_t base = moduleBase(moduleName);
    size_t size = moduleSize(moduleName);
    if (!handle_ || !base || !size) return out;

    uintptr_t addr = base;
    uintptr_t end = base + size;
    while (addr < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQueryEx(handle_, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)))
            break;

        uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        size_t regionSize = mbi.RegionSize;
        if (regionSize == 0) break;

        const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & writable))
            out.push_back({regionBase, regionSize});

        addr = regionBase + regionSize;
    }
    return out;
}

uintptr_t Process::scan(const char* pattern) const {    return scan(base_, moduleSize(off::ProcessName), pattern);
}

uintptr_t Process::scan(uintptr_t start, size_t span, const char* pattern) const {
    if (!handle_ || !start || !span) return 0;

    // tokenise
    std::vector<std::pair<uint8_t, bool>> tokens;
    const char* p = pattern;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        char buf[3]{};
        int n = 0;
        while (*p && *p != ' ' && n < 2) buf[n++] = *p++;
        uint8_t v{};
        bool wild{};
        if (!parseToken(buf, v, wild)) return 0;
        tokens.emplace_back(v, wild);
    }
    if (tokens.empty()) return 0;

    const size_t patLen = tokens.size();
    constexpr size_t kChunk = 0x100000; // 1 MiB windows keep reads bounded
    std::vector<uint8_t> buf(kChunk + patLen);

    for (size_t off = 0; off + patLen < span; off += kChunk) {
        size_t want = std::min(kChunk + patLen, span - off);
        SIZE_T got = 0;
        if (!ReadProcessMemory(handle_, reinterpret_cast<LPCVOID>(start + off),
                               buf.data(), want, &got) || got < patLen)
            continue;

        for (size_t i = 0; i + patLen <= got; ++i) {
            bool hit = true;
            for (size_t j = 0; j < patLen; ++j) {
                if (!tokens[j].second && buf[i + j] != tokens[j].first) {
                    hit = false;
                    break;
                }
            }
            if (hit) return start + off + i;
        }
    }
    return 0;
}

bool Process::readStdString(uintptr_t address, std::string& out) const {
    out.clear();
    // MSVC std::string: union buf[16] @0 | size @0x10 | capacity @0x18
    uintptr_t cap = read<uintptr_t>(address + 0x18);
    size_t size = read<size_t>(address + 0x10);

    if (size == 0) return true;
    if (size > 512) return false; // corrupt or not a string

    if (cap > 0xF) {
        uintptr_t ptr = read<uintptr_t>(address);
        if (!ptr) return false;
        out.resize(size);
        return readRaw(ptr, out.data(), size);
    }
    if (size > 15) return false; // claims SSO but does not fit the inline buffer
    char local[16]{};
    if (!readRaw(address, local, 16)) return false;
    out.assign(local, size);
    return true;
}

} // namespace mem
