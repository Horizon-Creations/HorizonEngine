#pragma once
#include <vector>
#include <cstdint>
#include <cassert>

struct SlotHandle {
    uint32_t index;
    uint32_t generation;
};

template<typename T>
class SlotMap {
public:
    SlotHandle insert(T value) {
        uint32_t slotIndex;
        if (!freeList_.empty()) {
            slotIndex = freeList_.back();
            freeList_.pop_back();
        } else {
            slotIndex = static_cast<uint32_t>(slots_.size());
            slots_.push_back({});
        }

        uint32_t dataIndex = static_cast<uint32_t>(data_.size());
        data_.push_back(std::move(value));
        erase_.push_back(slotIndex);

        slots_[slotIndex].dataIndex  = dataIndex;
        slots_[slotIndex].generation++;

        return { slotIndex, slots_[slotIndex].generation };
    }

    T* get(SlotHandle handle) {
        if (handle.index >= slots_.size()) return nullptr;
        Slot& slot = slots_[handle.index];
        if (slot.generation != handle.generation) return nullptr;
        return &data_[slot.dataIndex];
    }

    const T* get(SlotHandle handle) const {
        if (handle.index >= slots_.size()) return nullptr;
        const Slot& slot = slots_[handle.index];
        if (slot.generation != handle.generation) return nullptr;
        return &data_[slot.dataIndex];
    }

    bool isValid(SlotHandle handle) const {
        return handle.index < slots_.size() &&
               slots_[handle.index].generation == handle.generation;
    }

    void remove(SlotHandle handle) {
        if (!isValid(handle)) return;

        uint32_t dataIndex = slots_[handle.index].dataIndex;
        uint32_t lastData  = static_cast<uint32_t>(data_.size()) - 1;

        if (dataIndex != lastData) {
            data_[dataIndex]  = std::move(data_[lastData]);
            erase_[dataIndex] = erase_[lastData];
            slots_[erase_[dataIndex]].dataIndex = dataIndex;
        }

        data_.pop_back();
        erase_.pop_back();

        slots_[handle.index].generation++;
        freeList_.push_back(handle.index);
    }

    T*       begin()       { return data_.data(); }
    T*       end()         { return data_.data() + data_.size(); }
    const T* begin() const { return data_.data(); }
    const T* end()   const { return data_.data() + data_.size(); }

    uint32_t size()  const { return static_cast<uint32_t>(data_.size()); }
    bool     empty() const { return data_.empty(); }

    void clear() {
        data_.clear();
        erase_.clear();
        freeList_.clear();
        for (auto& s : slots_) s.generation++;
    }

private:
    struct Slot {
        uint32_t dataIndex  = 0;
        uint32_t generation = 1;
    };

    std::vector<Slot>     slots_;
    std::vector<T>        data_;
    std::vector<uint32_t> erase_;
    std::vector<uint32_t> freeList_;
};
