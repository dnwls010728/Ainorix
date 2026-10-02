#pragma once
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <vector>

namespace oe {
// Address-stable allocations. Images retain only their live blocks; subsequent
// frees/reallocations cannot invalidate pointers inside a captured VM/world.
class SnapshotHeap {
    struct Block {
        void* data; size_t size;
        explicit Block(size_t n) : data(::operator new(n, std::align_val_t(32))), size(n) {}
        ~Block() { ::operator delete(data, std::align_val_t(32)); }
    };
public:
    struct Image {
        struct Entry { std::shared_ptr<Block> block; std::vector<unsigned char> bytes; };
        std::vector<Entry> entries;
        size_t Bytes() const { size_t n = 0; for (const auto& e : entries) n += e.bytes.size(); return n; }
    };
    void* Allocate(size_t n) {
        auto b = std::make_shared<Block>(n); void* p = b->data;
        live_[p] = std::move(b); return p;
    }
    void Free(void* p) { live_.erase(p); }
    void* Reallocate(void* p, size_t n) {
        if (!n) { Free(p); return nullptr; }
        void* q = Allocate(n);
        if (p) { auto i = live_.find(p); if (i != live_.end()) std::memcpy(q, p, std::min(n, i->second->size)); Free(p); }
        return q;
    }
    Image Capture() const {
        Image image; image.entries.reserve(live_.size());
        for (const auto& item : live_) {
            const auto* p = static_cast<const unsigned char*>(item.first);
            image.entries.push_back({item.second, std::vector<unsigned char>(p, p + item.second->size)});
        }
        return image;
    }
    void Restore(const Image& image) {
        live_.clear();
        for (const auto& e : image.entries) { std::memcpy(e.block->data, e.bytes.data(), e.bytes.size()); live_[e.block->data] = e.block; }
    }
private:
    std::map<void*, std::shared_ptr<Block>> live_;
};
}  // namespace oe
