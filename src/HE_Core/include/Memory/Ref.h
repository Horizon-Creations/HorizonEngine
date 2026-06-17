#pragma once
#include <atomic>
#include <utility>

class RefCounted {
public:
    void acquire() noexcept { m_refCount.fetch_add(1, std::memory_order_relaxed); }
    void release() noexcept {
        if (m_refCount.fetch_sub(1, std::memory_order_acq_rel) == 1)
            delete this;
    }
    int useCount() const noexcept { return m_refCount.load(std::memory_order_relaxed); }
protected:
    RefCounted()  = default;
    virtual ~RefCounted() = default;  // virtual: delete-through-base in release()
    RefCounted(const RefCounted&) noexcept : m_refCount(0) {}
    RefCounted& operator=(const RefCounted&) noexcept { return *this; }
private:
    std::atomic<int> m_refCount{0};
};

template<typename T>
class Ref {
public:
    Ref() = default;
    explicit Ref(T* p) : m_ptr(p) { if (m_ptr) m_ptr->acquire(); }
    Ref(const Ref& o) : m_ptr(o.m_ptr) { if (m_ptr) m_ptr->acquire(); }
    Ref(Ref&& o) noexcept : m_ptr(o.m_ptr) { o.m_ptr = nullptr; }
    ~Ref() { if (m_ptr) m_ptr->release(); }

    Ref& operator=(const Ref& o) {
        if (&o != this) {
            T* old = m_ptr; m_ptr = o.m_ptr;
            if (m_ptr) m_ptr->acquire();
            if (old)   old->release();
        }
        return *this;
    }
    Ref& operator=(Ref&& o) noexcept {
        if (&o != this) {
            if (m_ptr) m_ptr->release();
            m_ptr = o.m_ptr; o.m_ptr = nullptr;
        }
        return *this;
    }

    T* get()        const noexcept { return m_ptr; }
    T* operator->() const noexcept { return m_ptr; }
    T& operator*()  const noexcept { return *m_ptr; }
    explicit operator bool() const noexcept { return m_ptr != nullptr; }

    void reset() { if (m_ptr) { m_ptr->release(); m_ptr = nullptr; } }

    bool operator==(const Ref& o) const noexcept { return m_ptr == o.m_ptr; }
    bool operator!=(const Ref& o) const noexcept { return m_ptr != o.m_ptr; }

private:
    T* m_ptr = nullptr;
};

template<typename T, typename... Args>
Ref<T> makeRef(Args&&... args) {
    return Ref<T>(new T(std::forward<Args>(args)...));
}
