#include "uuid.h"

#include <cstdio>
#include <mutex>
#include <random>

namespace agui {

// [device] Upstream used `thread_local std::mt19937` (a per-thread, lock-free generator). On
// ESP-IDF every thread_local byte is reserved from EACH task's stack at task creation, so a ~2.5 KB
// mt19937 TLS block overflows small system-task stacks (e.g. the ~1.5 KB idle task) and corrupts the
// scheduler at startup (boot loop). Use one shared generator guarded by a mutex instead — UUID
// generation is infrequent, so the lock cost is negligible. See ../PATCHES.md.
static std::mutex& generatorMutex() {
    static std::mutex m;
    return m;
}

static std::mt19937& getGenerator() {
    static std::mt19937 generator(std::random_device{}());
    return generator;
}

std::string UuidGenerator::generate() {
    std::lock_guard<std::mutex> lock(generatorMutex());
    std::uniform_int_distribution<uint32_t> dist32(0, 0xFFFFFFFF);
    std::uniform_int_distribution<uint16_t> dist16(0, 0xFFFF);
    auto& gen = getGenerator();

    uint32_t p1 = dist32(gen);
    uint16_t p2 = dist16(gen);
    uint16_t p3 = static_cast<uint16_t>((dist16(gen) & 0x0FFFu) | 0x4000u);  // version = 4
    uint16_t p4 = static_cast<uint16_t>((dist16(gen) & 0x3FFFu) | 0x8000u);  // variant = 10xx
    uint32_t p5h = dist32(gen);
    uint16_t p5l = dist16(gen);

    char uuid[37];
    snprintf(uuid, sizeof(uuid), "%08x-%04x-%04x-%04x-%08x%04x",
             p1, p2, p3, p4, p5h, static_cast<uint32_t>(p5l));
    return std::string(uuid);
}

}  // namespace agui
