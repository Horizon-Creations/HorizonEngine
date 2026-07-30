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
        if (!m_freeList.empty()) {
            slotIndex = m_freeList.back();
            m_freeList.pop_back();
        } else {
            slotIndex = static_cast<uint32_t>(m_slots.size());
            m_slots.push_back({});
        }

        uint32_t dataIndex = static_cast<uint32_t>(m_data.size());
        m_data.push_back(std::move(value));
        m_erase.push_back(slotIndex);

        m_slots[slotIndex].dataIndex  = dataIndex;
        m_slots[slotIndex].generation++;

        return { slotIndex, m_slots[slotIndex].generation };
    }

    T* get(SlotHandle handle) {
        if (handle.index >= m_slots.size()) return nullptr;
        Slot& slot = m_slots[handle.index];
        if (slot.generation != handle.generation) return nullptr;
        return &m_data[slot.dataIndex];
    }

    const T* get(SlotHandle handle) const {
        if (handle.index >= m_slots.size()) return nullptr;
        const Slot& slot = m_slots[handle.index];
        if (slot.generation != handle.generation) return nullptr;
        return &m_data[slot.dataIndex];
    }

    bool isValid(SlotHandle handle) const {
        return handle.index < m_slots.size() &&
               m_slots[handle.index].generation == handle.generation;
    }

    void remove(SlotHandle handle) {
        if (!isValid(handle)) return;

        uint32_t dataIndex = m_slots[handle.index].dataIndex;
        uint32_t lastData  = static_cast<uint32_t>(m_data.size()) - 1;

        if (dataIndex != lastData) {
            m_data[dataIndex]  = std::move(m_data[lastData]);
            m_erase[dataIndex] = m_erase[lastData];
            m_slots[m_erase[dataIndex]].dataIndex = dataIndex;
        }

        m_data.pop_back();
        m_erase.pop_back();

        m_slots[handle.index].generation++;
        m_freeList.push_back(handle.index);
    }

    T*       begin()       { return m_data.data(); }
    T*       end()         { return m_data.data() + m_data.size(); }
    const T* begin() const { return m_data.data(); }
    const T* end()   const { return m_data.data() + m_data.size(); }

    uint32_t size()  const { return static_cast<uint32_t>(m_data.size()); }
    bool     empty() const { return m_data.empty(); }

    void clear() {
        m_data.clear();
        m_erase.clear();
        m_freeList.clear();
        for (auto& s : m_slots) s.generation++;
    }

private:
    struct Slot {
        uint32_t dataIndex  = 0;
        uint32_t generation = 1;
    };

    std::vector<Slot>     m_slots;
    std::vector<T>        m_data;
    std::vector<uint32_t> m_erase;
    std::vector<uint32_t> m_freeList;
};
