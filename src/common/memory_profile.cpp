// Copyright (c) 2018-present Baidu, Inc. All Rights Reserved.
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "memory_profile.h"
#ifdef BAIKAL_TCMALLOC
#include <gperftools/malloc_extension.h>
#endif

namespace baikaldb {

DEFINE_int64(memory_gc_interval_s, 10, "mempry GC interval , default: 10s");
DEFINE_int64(memory_stats_interval_s, 60, "mempry GC interval , default: 60s");
DEFINE_int64(memory_free_rate, 20, "mempry free rate , default: 20");
DEFINE_int64(min_memory_use_size, 8589934592, "minimum memory use size , default: 8G");
DEFINE_int64(min_memory_free_size_to_release, 2147483648, "minimum memory free size to release, default: 2G");
DEFINE_int64(mem_tracker_gc_interval_s, 60, "do memory limit when row number more than #, default: 60");

void MemoryGCHandler::memory_gc_thread() {
#ifdef BAIKAL_TCMALLOC
    char stats_buffer[1000] = {0};
    const size_t BYTES_TO_GC = 8 * 1024 * 1024;
    TimeCost stats_cost;
    while (!_shutdown) {
        size_t used_size = 0;
        size_t free_size = 0;

        TimeCost cost;
        MallocExtension::instance()->GetNumericProperty("generic.current_allocated_bytes",
                                                        &used_size);
        MallocExtension::instance()->GetNumericProperty("tcmalloc.pageheap_free_bytes", &free_size);
        if (stats_cost.get_time() > FLAGS_memory_stats_interval_s * 1000 * 1000) {
            MallocExtension::instance()->GetStats(stats_buffer, sizeof(stats_buffer));
            size_t len = strlen(stats_buffer);
            size_t split_size = 1800;
            for (size_t i = 0; i < len; i += split_size) {
                char tmp = '\0';
                if (i + split_size < sizeof(stats_buffer)) {
                    tmp = stats_buffer[i + split_size];
                    stats_buffer[i + split_size]  = '\0';
                }
                SQL_TRACE("tcmalloc stats:\n%s", stats_buffer + i);
                if (i + split_size < sizeof(stats_buffer)) {
                    stats_buffer[i + split_size]  = tmp;
                }
            }
            stats_cost.reset();
        }
        size_t alloc_size = used_size + free_size;

        if ((int64_t)alloc_size > FLAGS_min_memory_use_size) {
            if (free_size > FLAGS_min_memory_free_size_to_release) {
                size_t total_bytes_to_gc = free_size - FLAGS_min_memory_free_size_to_release;
                size_t bytes = total_bytes_to_gc;
                while (bytes > BYTES_TO_GC) {
                    MallocExtension::instance()->ReleaseToSystem(BYTES_TO_GC);
                    bytes -= BYTES_TO_GC;
                }
                DB_WARNING("tcmalloc release memory about size: %ld cast: %ld", total_bytes_to_gc, cost.get_time());
            }
        }
        bthread_usleep_fast_shutdown(FLAGS_memory_gc_interval_s * 1000 * 1000LL, _shutdown);
    }
#endif
}

void MemTrackerPool::tracker_gc_thread() {
    while (!_shutdown) {
        bthread_usleep_fast_shutdown(FLAGS_memory_gc_interval_s * 1000 * 1000LL, _shutdown);
        std::map<uint64_t, SmartMemTracker> need_erase;
        _mem_tracker_pool.traverse_with_key_value([&need_erase](const uint64_t& log_id, SmartMemTracker& mem_tracker) {
            if (butil::gettimeofday_us() - mem_tracker->last_active_time() > FLAGS_mem_tracker_gc_interval_s * 1000 * 1000LL) {
                need_erase[log_id] = mem_tracker;
            }
        });
        for (auto& iter : need_erase) {
            if (butil::gettimeofday_us() - iter.second->last_active_time() > FLAGS_mem_tracker_gc_interval_s * 1000 * 1000LL) {
                _mem_tracker_pool.erase(iter.first);
            }
        }
    }
}

} //namespace baikal
