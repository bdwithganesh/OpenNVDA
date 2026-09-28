#ifndef NVKMD_MACOS_SUBMIT_H
#define NVKMD_MACOS_SUBMIT_H

/* Include after nvkmd.h. Kept independent of macOS APIs for host tests. */
#define NVKMD_MACOS_EXEC_NO_PREFETCH (1u << 0)
#define NVKMD_MACOS_EXEC_MAX 64u

static inline bool
macos_exec_chains_valid(uint32_t count, const struct nvkmd_ctx_exec *execs)
{
   uint32_t chain = 0;
   for (uint32_t i = 0; i < count; i++) {
      if (++chain > NVKMD_MACOS_EXEC_MAX)
         return false;
      if (!execs[i].incomplete)
         chain = 0;
   }
   return chain == 0;
}

/* The kernel appends a semaphore packet after every batch. Never put that
 * packet between a method header and the indirect data which completes it.
 * Zero means an acquire prefix must be submitted separately, or the caller
 * supplied an overlong/incomplete chain (rejected before any submission).
 */
static inline uint32_t
macos_exec_batch_count(uint32_t remaining, const struct nvkmd_ctx_exec *execs,
                      uint32_t capacity)
{
   uint32_t count = remaining < capacity ? remaining : capacity;
   while (count && execs[count - 1].incomplete)
      count--;
   return count;
}

#endif
