#include "config.h"
#include "SharedMemory.h"
#include <sys/mman.h>
#include <wtf/RefPtr.h>

namespace WebCore {
// Raster buffers can live in the renderer's private address space. No kernel
// shared-memory capability or cross-process mapping is available yet.
RefPtr<SharedMemory> SharedMemory::allocate(size_t size) {
    if (!size) return nullptr;
    void *data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data == MAP_FAILED) return nullptr;
    Ref memory = adoptRef(*new SharedMemory);
    memory->m_data = data;
    memory->m_size = size;
    return memory;
}

SharedMemory::~SharedMemory() { munmap(m_data, m_size); }

RefPtr<SharedMemory> SharedMemory::map(Handle &&, Protection, CopyOnWrite) { return nullptr; }

std::optional<SharedMemory::Handle> SharedMemory::createHandle(Protection) { return std::nullopt; }
}
