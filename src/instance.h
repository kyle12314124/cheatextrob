#pragma once
#include "memory.h"
#include <string>
#include <vector>

namespace rb {

class Instance {
public:
    Instance() = default;
    explicit Instance(uintptr_t address) : address_(address) {}
    uintptr_t address() const { return address_; }
    explicit operator bool() const { return address_ != 0; }
    bool operator==(const Instance& o) const { return address_ == o.address_; }

    std::string name() const;
    std::string className() const;
    Instance parent() const;
    std::vector<Instance> children() const;

    Instance findFirstChild(const std::string& name) const;
    Instance findFirstChildOfClass(const std::string& className) const;
    Instance findFirstAncestorOfClass(const std::string& className) const;

    bool isA(const std::string& className) const;
    bool isValid() const;

private:
    uintptr_t address_ = 0;
};

// Service shorthands off the DataModel.
Instance dataModel();
uintptr_t resolvedDataModel();
uintptr_t discoveredFakeOffset(); // module-relative; 0 until a scan finds it
void forceRescan();
const char* matchedVersion(); // which KnownSlots row resolved, nullptr if none
Instance workspace();
Instance players();
Instance lighting();
Instance localPlayer();
Instance camera();

} // namespace rb
