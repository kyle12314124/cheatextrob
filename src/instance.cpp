#include "instance.h"
#include "offsets.h"
#include <cctype>
#include <cstring>
#include <vector>
#include <algorithm>

namespace rb {

namespace {
constexpr size_t kMaxChildren = 4000; // a corrupted list is a refused read
constexpr size_t kMaxNameLen  = 128;

bool isName(const std::string& s) {
    if (s.empty() || s.size() > kMaxNameLen) return false;
    for (unsigned char c : s)
        if (c < 0x20 || c > 0x7E) return false;
    return true;
}

bool plausible(uintptr_t p) {
    return p > 0x10000 && p < 0x00007FFFFFFFFFFFULL;
}

// Newer builds store Name as a pointer to std::string (a "NameContainer").
// Older ones inline it. Try every representation, pointer forms first: an
// inline std::string NEVER looks like a user-mode pointer, because its first
// eight bytes are printable ASCII.
bool readNameField(uintptr_t addr, std::string& out) {
    out.clear();

    uintptr_t p = mem::g.read<uintptr_t>(addr);
    if (plausible(p)) {
        if (mem::g.readStdString(p, out) && isName(out)) return true; // ptr -> std::string
        out.clear();

        char buf[80]{};
        if (mem::g.readRaw(p, buf, sizeof(buf) - 1)) {
            size_t n = strnlen(buf, sizeof(buf) - 1);
            if (n >= 1) {
                std::string s(buf, n);
                if (isName(s)) { out = s; return true; }
            }
        }

        uintptr_t q = mem::g.read<uintptr_t>(p);
        if (plausible(q) && mem::g.readStdString(q, out) && isName(out)) return true;
        out.clear();
    }

    if (mem::g.readStdString(addr, out) && isName(out)) return true;   // inline std::string
    out.clear();
    return false;
}
} // namespace

std::string Instance::name() const {
    std::string s;
    if (!address_) return s;
    readNameField(address_ + off::InstanceName, s);
    return s;
}

std::string Instance::className() const {
    if (!address_) return {};

    // ClassDescriptor* -> class name (std::string or Name pointer)
    uintptr_t desc = mem::g.read<uintptr_t>(address_ + off::InstanceDescriptor);
    if (desc) {
        std::string s;
        if (readNameField(desc + off::DescriptorClassName, s)) return s;
    }
    std::string s;
    if (readNameField(address_ + off::InstanceDescriptor, s)) return s;
    return {};
}

Instance Instance::parent() const {
    if (!address_) return {};
    return Instance(mem::g.read<uintptr_t>(address_ + off::InstanceParent));
}

bool Instance::isValid() const {
    return address_ != 0 && !className().empty();
}

std::vector<Instance> Instance::children() const {
    std::vector<Instance> out;
    if (!address_) return out;

    // Instance::Children is a pointer to a container holding {begin, end}.
    uintptr_t container = mem::g.read<uintptr_t>(address_ + off::InstanceChildren);
    if (!plausible(container)) return out;

    uintptr_t begin = mem::g.read<uintptr_t>(container + off::InstanceChildrenBegin);
    uintptr_t end   = mem::g.read<uintptr_t>(container + off::InstanceChildrenEnd);
    if (!plausible(begin) || end <= begin) return out;

    uintptr_t diff = end - begin;
    if (diff % sizeof(uintptr_t) != 0) return out;

    size_t count = diff / sizeof(uintptr_t);
    if (count == 0 || count > kMaxChildren) return out;

    std::vector<uintptr_t> ptrs(count);
    if (!mem::g.readRaw(begin, ptrs.data(), count * sizeof(uintptr_t)))
        return out;

    out.reserve(count);
    for (uintptr_t p : ptrs) {
        if (!plausible(p)) continue;
        // Only keep slots that name us as their parent; this filters out
        // stale entries, free-list links and anything else in that buffer.
        if (mem::g.read<uintptr_t>(p + off::InstanceParent) != address_) continue;
        out.emplace_back(p);
    }
    return out;
}

Instance Instance::findFirstChild(const std::string& target) const {
    for (const auto& c : children())
        if (c.name() == target) return c;
    return {};
}

Instance Instance::findFirstChildOfClass(const std::string& cls) const {
    for (const auto& c : children())
        if (c.className() == cls) return c;
    return {};
}

Instance Instance::findFirstAncestorOfClass(const std::string& cls) const {
    Instance cur = parent();
    for (int depth = 0; depth < 64 && cur; ++depth) {
        if (cur.className() == cls) return cur;
        cur = cur.parent();
    }
    return {};
}

bool Instance::isA(const std::string& cls) const {
    return className() == cls;
}

// ---- service resolution ----

namespace {

bool plausibleHeap(uintptr_t p, uintptr_t modBase, size_t modSize) {
    if (p < 0x10000) return false;
    if (p > 0x00007FFFFFFFFFFFULL) return false;
    if (p >= modBase && p < modBase + modSize) return false;
    return true;
}

// A real DataModel has Workspace and Players among its direct children.
bool validateDataModel(uintptr_t real) {
    if (!real) return false;

    bool haveWorkspace = false;
    bool havePlayers = false;

    Instance dm(real);
    auto kids = dm.children();
    if (kids.size() < 2) return false;

    size_t limit = kids.size() < 256 ? kids.size() : 256;
    for (size_t i = 0; i < limit; ++i) {
        std::string cls = kids[i].className();
        if (cls == "Workspace") haveWorkspace = true;
        if (cls == "Players") havePlayers = true;
        if (haveWorkspace && havePlayers) return true;
    }
    return false;
}

// ---- runtime DataModel discovery ----
// The dump offset dies every patch. Walk the module's writable regions for a
// slot whose target resolves to a DataModel, and remember the module offset.
uintptr_t g_fakeOffset = 0;   // 0 = not discovered yet
uintptr_t g_realCache = 0;
ULONGLONG g_lastAttempt = 0;
const char* g_matchedVersion = nullptr;

uintptr_t discoverFakeOffset(uintptr_t modBase, size_t modSize) {
    auto ranges = mem::g.moduleWritableRanges(off::ProcessName);
    if (ranges.empty()) return 0;

    int budget = 250000;                 // bounded probe count
    constexpr size_t kChunk = 8u << 20;  // 8 MiB read windows

    for (const auto& r : ranges) {
        if (r.size > (512u << 20)) continue;
        std::vector<uint8_t> buf(kChunk);

        for (size_t off = 0; off + 8 <= r.size; off += kChunk) {
            size_t want = std::min(kChunk, r.size - off);
            if (!mem::g.readRaw(r.base + off, buf.data(), want)) continue;

            for (size_t i = 0; i + 8 <= want; i += 8) {
                uintptr_t fake = 0;
                std::memcpy(&fake, buf.data() + i, sizeof(fake));
                if (!plausibleHeap(fake, modBase, modSize)) continue;
                if (--budget <= 0) return 0;

                uintptr_t real = mem::g.read<uintptr_t>(fake + off::RealDataModel);
                if (!plausibleHeap(real, modBase, modSize)) continue;
                if (validateDataModel(real))
                    return (r.base + off + i) - modBase;
            }
        }
    }
    return 0;
}

} // namespace

// ---- service cache ----
// Resolving a service means walking every DataModel child and parsing its
// class name (~140 instances). That was happening several times per frame,
// which capped the overlay at ~35 fps. Walk once, then reuse the addresses.
namespace {

struct ServiceCache {
    uintptr_t dm = 0;
    uintptr_t workspace = 0;
    uintptr_t players = 0;
    uintptr_t lighting = 0;
    uintptr_t camera = 0;
    ULONGLONG stamp = 0;
};

ServiceCache g_svc;

void refreshServices(uintptr_t dm) {
    g_svc = ServiceCache{};
    g_svc.dm = dm;
    g_svc.stamp = GetTickCount64();
    if (!dm) return;

    for (const auto& c : Instance(dm).children()) {
        std::string cls = c.className();
        if (cls == "Workspace") g_svc.workspace = c.address();
        else if (cls == "Players") g_svc.players = c.address();
        else if (cls == "Lighting") g_svc.lighting = c.address();
        if (g_svc.workspace && g_svc.players && g_svc.lighting) break;
    }
    if (g_svc.workspace)
        g_svc.camera = mem::g.read<uintptr_t>(g_svc.workspace + off::WorkspaceCamera);
}

void ensureServices() {
    uintptr_t dm = resolvedDataModel();
    if (!dm) {
        g_svc = ServiceCache{};
        return;
    }
    ULONGLONG now = GetTickCount64();
    if (g_svc.dm != dm || now - g_svc.stamp > 1000) refreshServices(dm);
}

} // namespace

uintptr_t discoveredFakeOffset() {
    return g_fakeOffset;
}

const char* matchedVersion() {
    return g_matchedVersion;
}

void forceRescan() {
    g_fakeOffset = 0;
    g_realCache = 0;
    g_lastAttempt = 0;
    g_matchedVersion = nullptr;
    g_svc = ServiceCache{};
}

uintptr_t resolvedDataModel() {
    uintptr_t modBase = mem::g.base();
    size_t modSize = mem::g.moduleSize(off::ProcessName);
    if (!modBase) return 0;

    ULONGLONG now = GetTickCount64();

    // A resolved DataModel is revalidated once a second, not on every call.
    // dataModel() is hit several times per frame; walking 140 children and
    // parsing their class names each time was pure overhead.
    if (g_realCache) {
        static ULONGLONG lastValidate = 0;
        if (lastValidate != 0 && now - lastValidate < 1000) return g_realCache;
        lastValidate = now;
        if (validateDataModel(g_realCache)) return g_realCache;
        g_realCache = 0;
        g_matchedVersion = nullptr;
    }

    // 1. known slots, newest build first
    for (const auto& slot : off::KnownSlots) {
        uintptr_t fake = mem::g.read<uintptr_t>(modBase + slot.pointer);
        if (!fake) continue;
        uintptr_t real = mem::g.read<uintptr_t>(fake + slot.realOffset);
        if (validateDataModel(real)) {
            g_realCache = real;
            g_matchedVersion = slot.version;
            return real;
        }
    }
    if (g_fakeOffset) {
        uintptr_t fake = mem::g.read<uintptr_t>(modBase + g_fakeOffset);
        if (fake) {
            uintptr_t real = mem::g.read<uintptr_t>(fake + off::RealDataModel);
            if (validateDataModel(real)) {
                g_realCache = real;
                return real;
            }
        }
    }

    // 3. scan, throttled
    constexpr ULONGLONG kScanIntervalMs = 60000;
    if (g_lastAttempt != 0 && now - g_lastAttempt < kScanIntervalMs) return g_realCache;
    g_lastAttempt = now;

    uintptr_t offset = discoverFakeOffset(modBase, modSize);
    if (offset) {
        g_fakeOffset = offset;
        uintptr_t fake = mem::g.read<uintptr_t>(modBase + offset);
        g_realCache = fake ? mem::g.read<uintptr_t>(fake + off::RealDataModel) : 0;
    }
    return g_realCache;
}

Instance dataModel() {
    return Instance(resolvedDataModel());
}

Instance workspace() {
    ensureServices();
    return Instance(g_svc.workspace);
}

Instance players() {
    ensureServices();
    return Instance(g_svc.players);
}

Instance lighting() {
    ensureServices();
    return Instance(g_svc.lighting);
}

Instance localPlayer() {
    ensureServices();
    if (!g_svc.players) return {};
    // Cheap to re-read every frame: respawns replace the local player.
    return Instance(mem::g.read<uintptr_t>(g_svc.players + off::PlayersLocal));
}

Instance camera() {
    ensureServices();
    if (g_svc.workspace) {
        uintptr_t cam = mem::g.read<uintptr_t>(g_svc.workspace + off::WorkspaceCamera);
        if (cam) {
            g_svc.camera = cam;
            return Instance(cam);
        }
    }
    return Instance(g_svc.camera);
}

} // namespace rb
