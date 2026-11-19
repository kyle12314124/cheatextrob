#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mem {

class Process {
public:
    bool attach(const wchar_t* processName);
    void detach();
    bool valid() const { return handle_ != nullptr; }
    uint32_t pid() const { return pid_; }
    uintptr_t base() const { return base_; }
    HANDLE handle() const { return handle_; }

    bool readRaw(uintptr_t address, void* buffer, size_t size) const;
    bool writeRaw(uintptr_t address, const void* buffer, size_t size) const;

    template <typename T>
    T read(uintptr_t address) const {
        T out{};
        readRaw(address, &out, sizeof(T));
        return out;
    }

    template <typename T>
    bool write(uintptr_t address, const T& value) const {
        return writeRaw(address, &value, sizeof(T));
    }

    uintptr_t moduleBase(const wchar_t* moduleName) const;
    size_t moduleSize(const wchar_t* moduleName) const;
    std::wstring modulePath(const wchar_t* moduleName) const;

    struct Range {
        uintptr_t base;
        size_t size;
    };
    // Committed, writable regions inside the module image (the .data family).
    std::vector<Range> moduleWritableRanges(const wchar_t* moduleName) const;

    // IDA-style pattern, e.g. "48 8B 05 ?? ?? ?? ?? 48 85 C0"
    uintptr_t scan(const char* pattern) const;
    uintptr_t scan(uintptr_t start, size_t span, const char* pattern) const;

    // Reads a std::string at `address` and copies to `out`. Returns false on
    // refusal (unreadable, corrupt size, or data pointing outside the process).
    bool readStdString(uintptr_t address, std::string& out) const;

private:
    HANDLE handle_ = nullptr;
    uint32_t pid_ = 0;
    uintptr_t base_ = 0;
};

extern Process g;

} // namespace mem
