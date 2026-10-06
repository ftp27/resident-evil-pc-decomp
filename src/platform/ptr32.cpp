// ptr32.cpp - anchor for 32-bit pointer slots on 64-bit hosts (see ptr32.h).
#include "ptr32.h"

#if !RE1_PTR32_NATIVE

#include <stdio.h>
#include <stdlib.h>

// Never pointed at by anything, so offset 0 is free to mean NULL. 64-byte
// alignment keeps the low bits of an offset equal to those of its address.
alignas(64) char g_ptr32Anchor[64];

void ptr32_out_of_range(const void* p)
{
    fprintf(stderr,
            "ptr32: %p is outside the +/-2 GB window around the image anchor %p "
            "and cannot be stored in a 32-bit slot\n",
            p, (void*)g_ptr32Anchor);
    abort();
}

// ---------------------------------------------------------------------------
// Image-resident heap.
//
// The Marni layer allocates its objects and pixel/vertex buffers through
// operator_new and stores the results in 32-bit slots (CMarniBits,
// CDirect3DObject, PSXTexture, ...), so on a 64-bit host they have to come
// from inside the image as well. This is a zero-fill block (only pages that
// get touched are committed) carved into power-of-two size classes with one
// free list per class. Single-threaded in practice; the lock is cheap
// insurance against an audio-thread allocation.
// ---------------------------------------------------------------------------

#include <mutex>
#include <string.h>

namespace {

const size_t kHeapSize   = (size_t)768 << 20;   // 768 MB of address space
const int    kMinShift   = 4;                   // 16-byte blocks and up
const int    kClassCount = 32;
const size_t kHeader     = 16;                  // keeps payloads 16-aligned

alignas(4096) char s_heap[kHeapSize];
size_t             s_heapTop = 0;
void*              s_freeList[kClassCount];
std::mutex         s_heapLock;

int SizeClass(size_t total)
{
    int c = 0;
    while (((size_t)1 << (c + kMinShift)) < total) c++;
    return c;
}

}  // namespace

void* ptr32_alloc(size_t size)
{
    const int c = SizeClass(size + kHeader);
    if (c >= kClassCount) return NULL;
    std::lock_guard<std::mutex> lock(s_heapLock);

    char* block = (char*)s_freeList[c];
    if (block != NULL) {
        s_freeList[c] = *(void**)block;
    } else {
        const size_t bytes = (size_t)1 << (c + kMinShift);
        if (s_heapTop + bytes > kHeapSize) {
            fprintf(stderr, "ptr32: image heap exhausted (%zu bytes requested)\n", size);
            abort();
        }
        block = s_heap + s_heapTop;
        s_heapTop += bytes;
    }
    *(int*)block = c;
    return block + kHeader;
}

void ptr32_free(void* p)
{
    if (p == NULL) return;
    char* block = (char*)p - kHeader;
    const int c = *(int*)block;
    std::lock_guard<std::mutex> lock(s_heapLock);
    *(void**)block = s_freeList[c];
    s_freeList[c] = block;
}

#endif
