// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/cppgc/heap-base.h"

#include "include/cppgc/heap-consistency.h"
#include "src/base/platform/platform.h"
#include "src/base/sanitizer/lsan-page-allocator.h"
#include "src/heap/base/stack.h"
#include "src/heap/cppgc/globals.h"
#include "src/heap/cppgc/heap-object-header.h"
#include "src/heap/cppgc/heap-page.h"
#include "src/heap/cppgc/heap-statistics-collector.h"
#include "src/heap/cppgc/heap-visitor.h"
#include "src/heap/cppgc/marking-verifier.h"
#include "src/heap/cppgc/object-view.h"
#include "src/heap/cppgc/page-memory.h"
#include "src/heap/cppgc/platform.h"
#include "src/heap/cppgc/prefinalizer-handler.h"
#include "src/heap/cppgc/stats-collector.h"

namespace cppgc {
namespace internal {

namespace {

class ObjectSizeCounter : private HeapVisitor<ObjectSizeCounter> {
  friend class HeapVisitor<ObjectSizeCounter>;

 public:
  size_t GetSize(RawHeap& heap) {
    Traverse(heap);
    return accumulated_size_;
  }

 private:
  static size_t ObjectSize(const HeapObjectHeader& header) {
    return ObjectView<>(header).Size();
  }

  bool VisitHeapObjectHeader(HeapObjectHeader& header) {
    if (header.IsFree()) return true;
    accumulated_size_ += ObjectSize(header);
    return true;
  }

  size_t accumulated_size_ = 0;
};

}  // namespace

HeapBase::HeapBase(
    std::shared_ptr<cppgc::Platform> platform,
    const std::vector<std::unique_ptr<CustomSpaceBase>>& custom_spaces,
    StackSupport stack_support, MarkingType marking_support,
    SweepingType sweeping_support)
    : raw_heap_(this, custom_spaces),
      platform_(std::move(platform)),
      oom_handler_(std::make_unique<FatalOutOfMemoryHandler>(this)),
#if defined(LEAK_SANITIZER)
      lsan_page_allocator_(std::make_unique<v8::base::LsanPageAllocator>(
          platform_->GetPageAllocator())),
#endif  // LEAK_SANITIZER
#if defined(CPPGC_CAGED_HEAP)
      caged_heap_(*this, *page_allocator()),
      page_backend_(std::make_unique<PageBackend>(caged_heap_.allocator(),
                                                  *oom_handler_.get())),
#else   // !CPPGC_CAGED_HEAP
      page_backend_(std::make_unique<PageBackend>(*page_allocator(),
                                                  *oom_handler_.get())),
#endif  // !CPPGC_CAGED_HEAP
      stats_collector_(std::make_unique<StatsCollector>(platform_.get())),
      stack_(std::make_unique<heap::base::Stack>(
          v8::base::Stack::GetStackStart())),
      prefinalizer_handler_(std::make_unique<PreFinalizerHandler>(*this)),
      compactor_(raw_heap_),
      object_allocator_(raw_heap_, *page_backend_, *stats_collector_,
                        *prefinalizer_handler_),
      sweeper_(*this),
      strong_persistent_region_(*oom_handler_.get()),
      weak_persistent_region_(*oom_handler_.get()),
      strong_cross_thread_persistent_region_(*oom_handler_.get()),
      weak_cross_thread_persistent_region_(*oom_handler_.get()),
#if defined(CPPGC_YOUNG_GENERATION)
      remembered_set_(*this),
#endif  // defined(CPPGC_YOUNG_GENERATION)
      stack_support_(stack_support),
      marking_support_(marking_support),
      sweeping_support_(sweeping_support) {
  stats_collector_->RegisterObserver(
      &allocation_observer_for_PROCESS_HEAP_STATISTICS_);
}

HeapBase::~HeapBase() = default;

PageAllocator* HeapBase::page_allocator() const {
#if defined(LEAK_SANITIZER)
  return lsan_page_allocator_.get();
#else   // !LEAK_SANITIZER
  return platform_->GetPageAllocator();
#endif  // !LEAK_SANITIZER
}

size_t HeapBase::ObjectPayloadSize() const {
  return ObjectSizeCounter().GetSize(const_cast<RawHeap&>(raw_heap()));
}

size_t HeapBase::ExecutePreFinalizers() {
#ifdef CPPGC_ALLOW_ALLOCATIONS_IN_PREFINALIZERS
  // Allocations in pre finalizers should not trigger another GC.
  cppgc::subtle::NoGarbageCollectionScope no_gc_scope(*this);
#else
  // Pre finalizers are forbidden from allocating objects.
  cppgc::subtle::DisallowGarbageCollectionScope no_gc_scope(*this);
#endif  // CPPGC_ALLOW_ALLOCATIONS_IN_PREFINALIZERS
  prefinalizer_handler_->InvokePreFinalizers();
  return prefinalizer_handler_->ExtractBytesAllocatedInPrefinalizers();
}

#if defined(CPPGC_YOUNG_GENERATION)
void HeapBase::ResetRememberedSet() {
  class AllLABsAreEmpty final : protected HeapVisitor<AllLABsAreEmpty> {
    friend class HeapVisitor<AllLABsAreEmpty>;

   public:
    explicit AllLABsAreEmpty(RawHeap& raw_heap) { Traverse(raw_heap); }

    bool value() const { return !some_lab_is_set_; }

   protected:
    bool VisitNormalPageSpace(NormalPageSpace& space) {
      some_lab_is_set_ |=
          static_cast<bool>(space.linear_allocation_buffer().size());
      return true;
    }

   private:
    bool some_lab_is_set_ = false;
  };
  DCHECK(AllLABsAreEmpty(raw_heap()).value());
  caged_heap().local_data().age_table.Reset(&caged_heap().allocator());
  remembered_set_.Reset();
}
#endif  // defined(CPPGC_YOUNG_GENERATION)

void HeapBase::Terminate() {
  DCHECK(!IsMarking());
  CHECK(!in_disallow_gc_scope());

  sweeper().FinishIfRunning();

  constexpr size_t kMaxTerminationGCs = 20;
  size_t gc_count = 0;
  bool more_termination_gcs_needed = false;
  do {
    CHECK_LT(gc_count++, kMaxTerminationGCs);

    // Clear root sets.
    strong_persistent_region_.ClearAllUsedNodes();
    weak_persistent_region_.ClearAllUsedNodes();
    {
      PersistentRegionLock guard;
      strong_cross_thread_persistent_region_.ClearAllUsedNodes();
      weak_cross_thread_persistent_region_.ClearAllUsedNodes();
    }

    in_atomic_pause_ = true;
    stats_collector()->NotifyMarkingStarted(
        GarbageCollector::Config::CollectionType::kMajor,
        GarbageCollector::Config::IsForcedGC::kForced);
    object_allocator().ResetLinearAllocationBuffers();
    stats_collector()->NotifyMarkingCompleted(0);
    ExecutePreFinalizers();
    sweeper().Start(
        {Sweeper::SweepingConfig::SweepingType::kAtomic,
         Sweeper::SweepingConfig::CompactableSpaceHandling::kSweep});
    in_atomic_pause_ = false;

    sweeper().NotifyDoneIfNeeded();
    more_termination_gcs_needed =
        strong_persistent_region_.NodesInUse() ||
        weak_persistent_region_.NodesInUse() || [this]() {
          PersistentRegionLock guard;
          return strong_cross_thread_persistent_region_.NodesInUse() ||
                 weak_cross_thread_persistent_region_.NodesInUse();
        }();
  } while (more_termination_gcs_needed);

  object_allocator().Terminate();
  disallow_gc_scope_++;

  CHECK_EQ(0u, strong_persistent_region_.NodesInUse());
  CHECK_EQ(0u, weak_persistent_region_.NodesInUse());
  CHECK_EQ(0u, strong_cross_thread_persistent_region_.NodesInUse());
  CHECK_EQ(0u, weak_cross_thread_persistent_region_.NodesInUse());
}

HeapStatistics HeapBase::CollectStatistics(
    HeapStatistics::DetailLevel detail_level) {
  if (detail_level == HeapStatistics::DetailLevel::kBrief) {
    return {stats_collector_->allocated_memory_size(),
            stats_collector_->resident_memory_size(),
            stats_collector_->allocated_object_size(),
            HeapStatistics::DetailLevel::kBrief,
            {},
            {}};
  }

  sweeper_.FinishIfRunning();
  object_allocator_.ResetLinearAllocationBuffers();
  return HeapStatisticsCollector().CollectDetailedStatistics(this);
}

}  // namespace internal
}  // namespace cppgc
