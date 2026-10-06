// ptr32.h - 32-bit pointer slots on 64-bit hosts.
//
// The original binary stores pointers in 4-byte slots everywhere: struct fields
// whose layout is pinned by static_asserts, DWORD tables, and offsets inside
// loaded files that get relocated in place (RDT, .dor, EMD/TMD, animations).
// On a 32-bit target a slot simply holds the address. On a 64-bit target the
// address no longer fits, so a slot holds a signed 32-bit offset from an anchor
// inside the executable image instead ("compressed pointer").
//
// That works because the game, like the original, keeps its data in static
// buffers (g_DataBuffer, entity/model storage, ...) and its code in the same
// image: everything a slot can point at lies within +/-2 GB of the anchor.
// A pointer outside that range (heap, stack, a library) cannot be stored in a
// slot; ptr32_encode traps on it instead of silently truncating.
//
//   uint32_t O(const void* p)   pointer -> slot value   (0 for NULL)
//   T*       P<T>(uint32_t v)   slot value -> pointer   (NULL for 0)
//   Ptr32<T>                    a 4-byte field that behaves like T*
//
// O() is linear (O(p + n) == O(p) + n), so relocating a file offset with
// `slot += O(base)` produces the slot value of `base + offset`, exactly like
// the original's `slot += (int)base`.
//
// On a 32-bit target O/P are the identity casts and Ptr32<T> is layout- and
// code-identical to T*, so the Windows and Linux -m32 builds are unaffected.
#pragma once

#include <stddef.h>
#include <stdint.h>

#if UINTPTR_MAX == 0xFFFFFFFFu

#define RE1_PTR32_NATIVE 1

inline uint32_t ptr32_encode(const void* p) { return (uint32_t)(uintptr_t)p; }
inline void*    ptr32_decode(uint32_t v)    { return (void*)(uintptr_t)v; }

#else

#define RE1_PTR32_NATIVE 0

// Defined in platform/ptr32.cpp. Aligned so that an offset has the same low
// bits as the address it encodes (code that masks slot values, e.g. `& ~3`,
// keeps working).
extern char g_ptr32Anchor[];

// Allocation from a heap inside the image, for memory whose address is kept in
// 32-bit slots (operator_new uses it on 64-bit hosts).
void* ptr32_alloc(size_t size);
void  ptr32_free(void* p);

// Out of line: reports the offending pointer and aborts.
[[noreturn]] void ptr32_out_of_range(const void* p);

inline uint32_t ptr32_encode(const void* p)
{
    if (p == NULL) return 0;
    const ptrdiff_t d = (const char*)p - g_ptr32Anchor;
    if (d < INT32_MIN || d > INT32_MAX) ptr32_out_of_range(p);
    return (uint32_t)(int32_t)d;
}

inline void* ptr32_decode(uint32_t v)
{
    return v == 0 ? NULL : (void*)(g_ptr32Anchor + (int32_t)v);
}

#endif

inline uint32_t O(const void* p) { return ptr32_encode(p); }

template <class T> inline T* P(uint32_t v) { return (T*)ptr32_decode(v); }

// Function pointers live in the image too.
template <class F> inline uint32_t OF(F* fn) { return ptr32_encode((const void*)fn); }
template <class F> inline F* PF(uint32_t v) { return (F*)ptr32_decode(v); }

// A 4-byte pointer field. Converts to and from T*, so `s->field[i]`,
// `s->field->x`, `s->field = buf` and `if (s->field)` read as before.
#if RE1_PTR32_NATIVE
template <class T> using Ptr32 = T*;
#else
#pragma pack(push, 1)
template <class T> struct Ptr32 {
    uint32_t v;

    Ptr32() = default;
    Ptr32(T* p) : v(ptr32_encode(p)) {}
    Ptr32& operator=(T* p) { v = ptr32_encode(p); return *this; }

    operator T*() const { return (T*)ptr32_decode(v); }
    T* get() const { return (T*)ptr32_decode(v); }
    T* operator->() const { return get(); }
    // `(DWORD)field` / `(int)field` is the raw slot value, as it was in the
    // original; `(uintptr_t)field` is the real address.
    explicit operator uint32_t() const { return v; }
    explicit operator int32_t() const { return (int32_t)v; }
    explicit operator uintptr_t() const { return (uintptr_t)get(); }
    explicit operator intptr_t() const { return (intptr_t)get(); }
    // `(OtherType*)field`
    template <class U> explicit operator U*() const { return (U*)get(); }

    Ptr32& operator+=(ptrdiff_t n) { return *this = get() + n; }
    Ptr32& operator-=(ptrdiff_t n) { return *this = get() - n; }
    Ptr32& operator++() { return *this = get() + 1; }
    T* operator++(int) { T* old = get(); *this = old + 1; return old; }
};
template <> struct Ptr32<void> {
    uint32_t v;

    Ptr32() = default;
    Ptr32(void* p) : v(ptr32_encode(p)) {}
    Ptr32& operator=(void* p) { v = ptr32_encode(p); return *this; }
    operator void*() const { return ptr32_decode(v); }
    void* get() const { return ptr32_decode(v); }
    explicit operator uint32_t() const { return v; }
    explicit operator int32_t() const { return (int32_t)v; }
    explicit operator uintptr_t() const { return (uintptr_t)get(); }
    explicit operator intptr_t() const { return (intptr_t)get(); }
    template <class U> explicit operator U*() const { return (U*)get(); }
};
#pragma pack(pop)
static_assert(sizeof(Ptr32<int>) == 4, "Ptr32 must stay 4 bytes");
#endif
