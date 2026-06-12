#pragma once
#include <Types/Handle.h>
#include <cstdint>

class GPUMemoryAllocator {
public:
    explicit GPUMemoryAllocator(uint64_t budgetBytes);

    bool requestAllocation(uint64_t sizeBytes, RenderHandle handle);
    void freeAllocation(RenderHandle handle);

    void onHandleUsed(RenderHandle handle);
    void evictLRU();

    uint64_t usedBytes()   const;
    uint64_t totalBudget() const;

private:
    uint64_t budgetBytes_;
    uint64_t usedBytes_ = 0;
};
