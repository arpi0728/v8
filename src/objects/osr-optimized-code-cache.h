// Copyright 2019 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_OBJECTS_OSR_OPTIMIZED_CODE_CACHE_H_
#define V8_OBJECTS_OSR_OPTIMIZED_CODE_CACHE_H_

#include "src/objects/fixed-array.h"
// Has to be the last include (doesn't have include guards):
#include "src/objects/object-macros.h"

namespace v8 {
namespace internal {

// This enum is a performance optimization for accessing the OSR code cache -
// we can skip cache iteration in many cases unless there are multiple entries
// for a particular SharedFunctionInfo.
enum OSRCodeCacheStateOfSFI : uint8_t {
  kNotCached,       // Likely state.
  kCachedOnce,      // Unlikely state, one entry.
  kCachedMultiple,  // Very unlikely state, multiple entries.
};

class V8_EXPORT OSROptimizedCodeCache : public WeakFixedArray {
 public:
  DECL_CAST(OSROptimizedCodeCache)

  enum OSRCodeCacheConstants {
    kSharedOffset,
    kCachedCodeOffset,
    kOsrIdOffset,
    kEntryLength
  };

  static const int kInitialLength = OSRCodeCacheConstants::kEntryLength * 4;
  static const int kMaxLength = OSRCodeCacheConstants::kEntryLength * 1024;

  // Caches the optimized code |code| corresponding to the shared function
  // |shared| and bailout id |osr_offset| in the OSROptimized code cache.
  // If the OSR code cache wasn't created before it creates a code cache with
  // kOSRCodeCacheInitialLength entries.
  static void AddOptimizedCode(Handle<NativeContext> context,
                               Handle<SharedFunctionInfo> shared,
                               Handle<CodeT> code, BytecodeOffset osr_offset);
  // Reduces the size of the OSR code cache if the number of valid entries are
  // less than the current capacity of the cache.
  static void Compact(Handle<NativeContext> context);
  // Sets the OSR optimized code cache to an empty array.
  static void Clear(NativeContext context);

  // Returns the code corresponding to the shared function |shared| and
  // BytecodeOffset |offset| if an entry exists in the cache. Returns an empty
  // object otherwise.
  CodeT GetOptimizedCode(Handle<SharedFunctionInfo> shared,
                         BytecodeOffset osr_offset, Isolate* isolate);

  // Remove all code objects marked for deoptimization from OSR code cache.
  void EvictMarkedCode(Isolate* isolate);

  // Returns vector of bytecode offsets corresponding to the shared function
  // |shared|
  std::vector<int> GetBytecodeOffsetsFromSFI(SharedFunctionInfo shared);

 private:
  // Functions that implement heuristics on when to grow / shrink the cache.
  static int CapacityForLength(int curr_capacity);
  static bool NeedsTrimming(int num_valid_entries, int curr_capacity);
  static int GrowOSRCache(Handle<NativeContext> native_context,
                          Handle<OSROptimizedCodeCache>* osr_cache);

  // Helper functions to get individual items from an entry in the cache.
  CodeT GetCodeFromEntry(int index);
  SharedFunctionInfo GetSFIFromEntry(int index);
  BytecodeOffset GetBytecodeOffsetFromEntry(int index);

  inline int FindEntry(Handle<SharedFunctionInfo> shared,
                       BytecodeOffset osr_offset);
  inline void ClearEntry(int src, Isolate* isolate);
  inline void InitializeEntry(int entry, SharedFunctionInfo shared, CodeT code,
                              BytecodeOffset osr_offset);
  inline void MoveEntry(int src, int dst, Isolate* isolate);

  OBJECT_CONSTRUCTORS(OSROptimizedCodeCache, WeakFixedArray);
};

}  // namespace internal
}  // namespace v8

#include "src/objects/object-macros-undef.h"

#endif  // V8_OBJECTS_OSR_OPTIMIZED_CODE_CACHE_H_
