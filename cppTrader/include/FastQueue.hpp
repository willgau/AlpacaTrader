#pragma once

// Fast Queue implementation by David Gross from cppcon 2025

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>


constexpr std::size_t kCacheLine = 64;

inline constexpr bool is_pow2(std::size_t x) noexcept {
    return x && ((x & (x - 1)) == 0);
}

template <std::size_t Align>
inline constexpr std::size_t align_up(std::size_t x) noexcept {
    static_assert((Align & (Align - 1)) == 0, "Align must be power of two");
    return (x + (Align - 1)) & ~(Align - 1);
}

//forces every instance of this struct to start at a 64-byte boundary in memory.
struct alignas(kCacheLine) PaddedAtomicU64 {
    std::atomic<std::uint64_t> v{ 0 };

    //Make the padding exactly large enough so that the entire struct occupies exactly one cache line.
    std::byte pad[kCacheLine - sizeof(std::atomic<std::uint64_t>)]{};
};
static_assert(sizeof(PaddedAtomicU64) == kCacheLine);

struct FastQueueStorage {
    //must deallocate with the matching aligned
    std::unique_ptr<std::byte, void(*)(void*)> buf{ nullptr, +[](void* p) {
       if (p)
       {
           ::operator delete(p, std::align_val_t(kCacheLine));
       }
    } };
    std::size_t capacity = 0;
    std::size_t mask = 0;
 };

template <std::size_t CapacityBytes, std::size_t BlockAlignment, std::size_t ReservePublishBlockBytes>
class FastQueue;

template <std::size_t CapacityBytes, std::size_t BlockAlignment, std::size_t ReservePublishBlockBytes>
class FastQueueProducer;

template <std::size_t CapacityBytes, std::size_t BlockAlignment, std::size_t ReservePublishBlockBytes>
class FastQueueConsumer;


template < std::size_t CapacityBytes, std::size_t BlockAlignment = 8, std::size_t ReservePublishBlockBytes = 1 << 16>
class FastQueue {
    static_assert(is_pow2(CapacityBytes), "CapacityBytes must be power-of-two");
    static_assert(is_pow2(BlockAlignment), "BlockAlignment must be power-of-two");
    static_assert(is_pow2(ReservePublishBlockBytes), "ReservePublishBlockBytes must be power-of-two");

public:
    static constexpr std::int32_t kWrapMarker = -1;

    FastQueue()
        : storage_(allocate_storage()) {
    }

    FastQueue(const FastQueue&) = delete;
    FastQueue& operator=(const FastQueue&) = delete;

    FastQueue(FastQueue&&) noexcept = default;
    FastQueue& operator=(FastQueue&&) noexcept = default;

    FastQueueProducer<CapacityBytes, BlockAlignment, ReservePublishBlockBytes> make_producer() noexcept {
        return FastQueueProducer<CapacityBytes, BlockAlignment, ReservePublishBlockBytes>(*this);
    }

    FastQueueConsumer<CapacityBytes, BlockAlignment, ReservePublishBlockBytes> make_consumer() noexcept {
        return FastQueueConsumer<CapacityBytes, BlockAlignment, ReservePublishBlockBytes>(*this);
    }

private:
    friend class FastQueueProducer<CapacityBytes, BlockAlignment, ReservePublishBlockBytes>;
    friend class FastQueueConsumer<CapacityBytes, BlockAlignment, ReservePublishBlockBytes>;

    PaddedAtomicU64 write_reserve_{};
    PaddedAtomicU64 write_commit_{};
    FastQueueStorage storage_;

    static FastQueueStorage allocate_storage() {
        FastQueueStorage s;
        s.capacity = CapacityBytes;
        s.mask = CapacityBytes - 1;

        void* p = ::operator new(CapacityBytes, std::align_val_t(kCacheLine));
        std::memset(p, 0, CapacityBytes);

        s.buf = { reinterpret_cast<std::byte*>(p), +[](void* q) {
            ::operator delete(q, std::align_val_t(kCacheLine));
        } };
        return s;
    }

    std::byte* ptr_at(std::uint64_t counter) noexcept {
        return storage_.buf.get() + (static_cast<std::size_t>(counter) & storage_.mask);
    }
};


template < std::size_t CapacityBytes, std::size_t BlockAlignment, std::size_t ReservePublishBlockBytes>
class FastQueueProducer {
public:
    explicit FastQueueProducer(FastQueue<CapacityBytes, BlockAlignment, ReservePublishBlockBytes>& q) noexcept
        : q_(&q) {
    }

    void write(std::span<const std::byte> payload) {
        write_impl(payload.size(), [&](std::span<std::byte> dst) {
            std::memcpy(dst.data(), payload.data(), payload.size());
            });
    }

    template <class F>
    void write_with(std::size_t payload_size, F&& fill) {
        write_impl(payload_size, std::forward<F>(fill));
    }

    std::uint64_t committed_bytes() const noexcept { return local_counter_; }

private:
    using Q = FastQueue<CapacityBytes, BlockAlignment, ReservePublishBlockBytes>;
    Q* q_;
    std::uint64_t local_counter_{ 0 };
    std::uint64_t cached_reserve_publish_{ 0 };

    void publish_reserve_if_needed(std::uint64_t new_counter) {
        if (cached_reserve_publish_ < new_counter) {
            cached_reserve_publish_ =
                align_up<ReservePublishBlockBytes>(static_cast<std::size_t>(new_counter));
            q_->write_reserve_.v.store(cached_reserve_publish_, std::memory_order_release);
        }
    }

    template <class F>
    void write_impl(std::size_t payload_size, F&& fill) {
        const std::size_t padded_payload = align_up<BlockAlignment>(payload_size);
        const std::size_t frame_bytes = sizeof(std::int32_t) + padded_payload;

        std::size_t pos = static_cast<std::size_t>(local_counter_) & q_->storage_.mask;

        if (pos + sizeof(std::int32_t) > CapacityBytes) {
            local_counter_ += (CapacityBytes - pos);
            pos = 0;
        }
        else if (pos + frame_bytes > CapacityBytes) {
            std::byte* p = q_->ptr_at(local_counter_);
            const std::int32_t marker = Q::kWrapMarker;
            std::memcpy(p, &marker, sizeof(marker));

            local_counter_ += (CapacityBytes - pos);
            publish_reserve_if_needed(local_counter_);
            q_->write_commit_.v.store(local_counter_, std::memory_order_release);

            pos = 0;
        }

        publish_reserve_if_needed(local_counter_ + frame_bytes);

        std::byte* base = q_->ptr_at(local_counter_);
        const std::int32_t sz = static_cast<std::int32_t>(payload_size);
        std::memcpy(base, &sz, sizeof(sz));

        std::span<std::byte> dst{ base + sizeof(std::int32_t), payload_size };
        fill(dst);

        if (padded_payload > payload_size) {
            std::memset(base + sizeof(std::int32_t) + payload_size, 0, padded_payload - payload_size);
        }

        local_counter_ += frame_bytes;
        q_->write_commit_.v.store(local_counter_, std::memory_order_release);
    }
};


template < std::size_t CapacityBytes, std::size_t BlockAlignment, std::size_t ReservePublishBlockBytes>
class FastQueueConsumer {
public:
    explicit FastQueueConsumer(FastQueue<CapacityBytes, BlockAlignment, ReservePublishBlockBytes>& q) noexcept
        : q_(&q) {
    }

    // Returns:
    //  >0 : bytes copied into dst
    //   0 : empty
    //  <0 : dst too small; required size is -return_value
    std::int32_t try_read(std::span<std::byte> dst) {
        using Q = FastQueue<CapacityBytes, BlockAlignment, ReservePublishBlockBytes>;

        const auto reserve = q_->write_reserve_.v.load(std::memory_order_acquire);
        assert(reserve - local_counter_ <= CapacityBytes && "queue overflow (consumer too slow)");

        if (local_counter_ == cached_commit_) {
            cached_commit_ = q_->write_commit_.v.load(std::memory_order_acquire);
        }
        if (local_counter_ == cached_commit_) return 0;

        std::size_t pos = static_cast<std::size_t>(local_counter_) & q_->storage_.mask;

        if (pos + sizeof(std::int32_t) > CapacityBytes) {
            local_counter_ += (CapacityBytes - pos);
            return try_read(dst);
        }

        std::byte* base = q_->ptr_at(local_counter_);

        std::int32_t sz = 0;
        std::memcpy(&sz, base, sizeof(sz));

        if (sz == Q::kWrapMarker) {
            local_counter_ += (CapacityBytes - pos);
            cached_commit_ = q_->write_commit_.v.load(std::memory_order_acquire);
            return try_read(dst);
        }

        if (sz < 0) {
            assert(false && "invalid frame size");
            return 0;
        }

        const std::size_t payload_size = static_cast<std::size_t>(sz);
        if (payload_size > dst.size()) {
            return -static_cast<std::int32_t>(payload_size);
        }

        const std::size_t padded_payload = align_up<BlockAlignment>(payload_size);
        const std::size_t frame_bytes = sizeof(std::int32_t) + padded_payload;

        std::memcpy(dst.data(), base + sizeof(std::int32_t), payload_size);
        local_counter_ += frame_bytes;

        return static_cast<std::int32_t>(payload_size);
    }

    std::uint64_t consumed_bytes() const noexcept { return local_counter_; }

private:
    using Q = FastQueue<CapacityBytes, BlockAlignment, ReservePublishBlockBytes>;
    Q* q_;
    std::uint64_t local_counter_{ 0 };
    std::uint64_t cached_commit_{ 0 };
};

