// B3 的内核共享内存对象：服务（如 time:u 的 GetSharedMemoryNativeHandle）交给客户端的共享内存句柄。句柄 0x9000 + 下标。
// 映射本身由 hle_svc.c 在 DM_B3 下的 SVC 0x13 MapSharedMemory / 0x14 UnmapSharedMemory 完成（那里登记 QueryMemory 区间）；这里只管对象表。
// 内容按服务文档填；文档没写明的保持全 0，并在日志里记一行，等看到客户端怎么用再补（不猜）。
// B3 kernel shared memory objects: shared memory handles that services (such as time:u GetSharedMemoryNativeHandle) give to the client. Handle 0x9000 + index.
// The mapping itself is done by hle_svc.c's SVC 0x13 MapSharedMemory / 0x14 UnmapSharedMemory under DM_B3 (which registers the QueryMemory range); this file only keeps the object table.
// Contents are filled from the service documentation; anything not documented stays all zero and is logged, to be filled once the client's use is seen (no guessing).
#include "dm.h"

#define B3_SHM_BASE 0x9000u
#define B3_SHM_MAX 32
typedef struct { int used; uint64_t size; uint64_t mapped; char tag[16]; } b3_shm_t;
static b3_shm_t g_shm[B3_SHM_MAX];
static pthread_mutex_t g_shm_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t b3_shmem_new(uint64_t size, const char *tag) {
    pthread_mutex_lock(&g_shm_lock);
    for (unsigned i = 1; i < B3_SHM_MAX; i++)
        if (!g_shm[i].used) {
            g_shm[i] = (b3_shm_t){1, size, 0, ""}; strlcpy(g_shm[i].tag, tag, sizeof g_shm[i].tag);
            pthread_mutex_unlock(&g_shm_lock);
            LOG("B3-SHMEM new tag=%s size=%#llx handle=%#x（内容全 0 | contents all zero）", tag, size, B3_SHM_BASE + i);
            return B3_SHM_BASE + i;
        }
    die("B3 共享内存对象表满 | B3 shared memory object table full");
}
static b3_shm_t *shm_get(uint32_t h) { uint32_t i = h - B3_SHM_BASE; return h >= B3_SHM_BASE && i < B3_SHM_MAX && g_shm[i].used ? &g_shm[i] : NULL; }
// 映射 / 解除映射时由 hle_svc.c 调：返回对象大小，0 = 句柄无效 | Called by hle_svc.c on map / unmap: returns the object size, 0 = invalid handle
uint64_t b3_shmem_size(uint32_t h) { pthread_mutex_lock(&g_shm_lock); b3_shm_t *s = shm_get(h); uint64_t n = s ? s->size : 0; pthread_mutex_unlock(&g_shm_lock); return n; }
void b3_hid_attach(void *base);   // b3_hid.c
void b3_shmem_mapped(uint32_t h, uint64_t addr) {
    pthread_mutex_lock(&g_shm_lock); b3_shm_t *s = shm_get(h); if (s) s->mapped = addr; pthread_mutex_unlock(&g_shm_lock);
    if (!s) return;
    LOG("B3-SHMEM map tag=%s handle=%#x", s->tag, h);
    if (!strcmp(s->tag, "hid")) b3_hid_attach((void *)(uintptr_t)addr);   // 写入掌机手柄并开始推输入 | write the handheld controller and start pushing input
    if (!strcmp(s->tag, "time")) { void b3_time_attach(void *base); b3_time_attach((void *)(uintptr_t)addr); }   // 写时钟上下文（存档时间）| write the clock contexts (save timestamps)
}
// CloseHandle：是共享内存句柄就关掉并返回 1 | CloseHandle: closes it and returns 1 if it is a shared memory handle
int b3_shmem_close(uint32_t h) { pthread_mutex_lock(&g_shm_lock); b3_shm_t *s = shm_get(h); if (s) s->used = 0; pthread_mutex_unlock(&g_shm_lock); return s != NULL; }

// ---------- 传输内存（SVC 0x15 CreateTransferMemory）：句柄 0xA000 + 下标，只记来源区间；服务（如 bsd:u）拿到后按需直接读写这段客体内存
// ---------- Transfer memory (SVC 0x15 CreateTransferMemory): handle 0xA000 + index, only the source range is recorded; a service (such as bsd:u) that receives it reads and writes that guest range directly as needed
#define B3_TMEM_BASE 0xA000u
typedef struct { int used; uint64_t addr, size; } b3_tmem_t;
static b3_tmem_t g_tmem[B3_SHM_MAX];
uint32_t b3_tmem_new(uint64_t addr, uint64_t size) {
    pthread_mutex_lock(&g_shm_lock);
    for (unsigned i = 1; i < B3_SHM_MAX; i++)
        if (!g_tmem[i].used) { g_tmem[i] = (b3_tmem_t){1, addr, size}; pthread_mutex_unlock(&g_shm_lock); LOG("B3-TMEM new size=%#llx handle=%#x", size, B3_TMEM_BASE + i); return B3_TMEM_BASE + i; }
    die("B3 传输内存对象表满 | B3 transfer memory object table full");
}
int b3_tmem_close(uint32_t h) {
    uint32_t i = h - B3_TMEM_BASE; int ok = h >= B3_TMEM_BASE && i < B3_SHM_MAX;
    pthread_mutex_lock(&g_shm_lock); ok = ok && g_tmem[i].used; if (ok) g_tmem[i].used = 0; pthread_mutex_unlock(&g_shm_lock); return ok;
}
