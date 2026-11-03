"""Run the production encoded-buffer cache with CPU-only Metal doubles."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RT = (ROOT / "nvn/nvn_rt_cache.inc").read_text()
FILE_STORE = RT[RT.index("typedef struct rt_free_span"):RT.index("// Both the snapshot writer")]
IO = RT[RT.index("static int rt_cache_io("):RT.index("// A canceled asynchronous read")]
PREFIX = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#define MLOG(...) ((void)0)
#define MDIE(...) abort()
static _Atomic uint64_t g_cur_frame;
static _Atomic int g_up_pending;
static uint64_t g_fence = 10, g_done = 10;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int submitting = 1, fail_write, fail_read;
static size_t wrote, read_bytes;
static int on_sub_thread(void) { return submitting; }
static int rt_cache_on(void) { return 1; }
static ssize_t test_write(int fd, const void *data, size_t size, off_t offset) {
    if (fail_write) { errno = ENOSPC; return -1; }
    ssize_t n = pwrite(fd, data, MIN(size, (size_t)32771), offset);
    if (n > 0) wrote += n;
    return n;
}
static ssize_t test_read(int fd, void *data, size_t size, off_t offset) {
    if (fail_read) { errno = EIO; return -1; }
    ssize_t n = pread(fd, data, MIN(size, (size_t)32771), offset);
    if (n > 0) read_bytes += n;
    return n;
}
#define pwrite test_write
#define pread test_read
@interface MockBuffer : NSObject <MTLBuffer>
@property(nonatomic, strong) NSMutableData *data;
@property(nonatomic) MTLStorageMode storageMode;
@property(nonatomic, strong) id<MTLHeap> heap;
@property(nonatomic) unsigned empty_count;
@end
@implementation MockBuffer
- (void *)contents { return self.data.mutableBytes; }
- (NSUInteger)length { return self.data.length; }
- (NSUInteger)allocatedSize { return (self.data.length + 16383u) & ~(NSUInteger)16383u; }
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)state {
    if (state == MTLPurgeableStateEmpty) {
        self.empty_count++;
        memset(self.contents, 0xEE, self.length);
    }
    return MTLPurgeableStateNonVolatile;
}
@end
'''
TESTS = r'''
static MockBuffer *make_buffer(size_t bytes) {
    MockBuffer *buffer = [MockBuffer new]; buffer.data = [NSMutableData dataWithLength:bytes];
    buffer.storageMode = MTLStorageModeShared;
    uint8_t *data = buffer.contents;
    for (size_t i = 0; i < bytes; i++) data[i] = (uint8_t)(i * 71 + i / 101);
    return buffer;
}
static void advance(void) {
    g_cur_frame++; wrote = 0; buffer_cache_trim();
    assert(wrote <= BUFFER_CACHE_WRITE_LIMIT);
}
static NvBufferCache *entry(MockBuffer *buffer) { return [g_buffer_cache objectForKey:buffer]; }
static void reset(void) {
    g_buffer_cache = nil; g_cur_frame += 1000; g_fence = g_done = 10;
    submitting = 1; fail_write = fail_read = 0; g_up_pending = 0;
}
static void spill(MockBuffer *buffer) {
    for (unsigned i = 0; !entry(buffer)->spilled; i++) { assert(i < 100); advance(); }
}
int main(void) { @autoreleasepool {
    reset();
    MockBuffer *large = make_buffer((9 << 20) + 7);
    NSData *expected = [large.data copy]; buffer_cache_track(large);
    g_done = 11; advance(); assert(entry(large)->fd < 0);
    g_done = 10; g_cur_frame += 200; advance(); assert(entry(large)->fd < 0);
    g_done = 11; g_up_pending = 1; advance(); assert(entry(large)->fd < 0);
    g_up_pending = 0; advance(); assert(entry(large)->written == BUFFER_CACHE_WRITE_LIMIT && !large.empty_count);
    // A use halfway through saving invalidates that partial snapshot, including
    // future writes, rather than ever discarding the current authoritative bytes.
    buffer_cache_use(large); assert(entry(large)->fd < 0);
    memset(large.contents, 0x3D, large.length); expected = [large.data copy];
    g_fence = 11; buffer_cache_use(large); g_cur_frame += 200; advance(); assert(entry(large)->fd < 0);
    g_done = 12; spill(large); assert(large.empty_count == 1);
    assert(entry(large)->file_bytes == large.length);
    id<MTLBuffer> alias = large;
    read_bytes = 0; buffer_cache_use(alias);
    assert([large.data isEqual:expected] && read_bytes == large.length);
    assert(!entry(large)->spilled && entry(large)->fd < 0);
    // A failed restore retains its backing and cannot silently return corrupt data.
    g_cur_frame += 200; spill(large); fail_read = 1;
    assert(!buffer_cache_restore(entry(large), large) && entry(large)->fd >= 0 && entry(large)->spilled);
    fail_read = 0; buffer_cache_use(large); assert([large.data isEqual:expected]);
    reset();
    MockBuffer *failed = make_buffer(12345); expected = [failed.data copy]; buffer_cache_track(failed);
    g_done = 11; g_cur_frame += 200; fail_write = 1; advance();
    assert(entry(failed)->excluded && entry(failed)->fd < 0 && !failed.empty_count && [failed.data isEqual:expected]);
    reset();
    MockBuffer *foreign = make_buffer(65536); buffer_cache_track(foreign);
    submitting = 0; buffer_cache_use(foreign); submitting = 1;
    g_done = 11; g_cur_frame += 200; advance(); assert(entry(foreign)->excluded && !foreign.empty_count);
    MockBuffer *private = make_buffer(64); private.storageMode = MTLStorageModePrivate;
    buffer_cache_track(private); assert(!entry(private));
    reset();
    NSMutableArray *many = [NSMutableArray new];
    for (unsigned i = 0; i < 13; i++) { MockBuffer *b = make_buffer(1000003); buffer_cache_track(b); [many addObject:b]; }
    g_done = 11; g_cur_frame += 200;
    for (unsigned i = 0; i < 4; i++) advance();
    for (MockBuffer *b in many) assert(entry(b)->spilled && b.empty_count == 1);
    NSDictionary *status = buffer_cache_status();
    assert([status[@"spilled"] unsignedIntValue] == 13 && ![status[@"nonpurgeable_allocated_bytes"] unsignedLongLongValue]);
    reset();
    __weak MockBuffer *released;
    @autoreleasepool {
        MockBuffer *temporary = make_buffer(20001); released = temporary;
        buffer_cache_track(temporary); g_done = 11; g_cur_frame += 200; spill(temporary);
        status = buffer_cache_status();
    }
    assert(!released && [status[@"spilled"] unsignedIntValue] == 1);
    // Purgeable storage ownership and diagnostics must not extend the buffer's lifetime.
    status = buffer_cache_status(); assert(![status[@"tracked"] unsignedIntValue]);
    puts("BUFFER_CACHE OK: fences, age, bounded I/O, exact aliases, cancellation, failures, ownership");
} return 0; }
'''
with tempfile.TemporaryDirectory(prefix="nvn-buffer-cache-") as directory:
    path = Path(directory)
    source = PREFIX + FILE_STORE + IO + (ROOT / "nvn/nvn_buffer_cache.inc").read_text() + TESTS
    (path / "test.m").write_text(source)
    command = ["xcrun", "clang", "-fobjc-arc", "-fblocks", "-Wno-protocol", "-Wno-objc-protocol-property-synthesis", "-Wall", "-Wextra",
               "-framework", "Foundation", "-framework", "Metal", str(path / "test.m"), "-o", str(path / "test")]
    if os.environ.get("NVN_TEST_SANITIZE"):
        command[2:2] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    subprocess.run(command, check=True)
    subprocess.run([str(path / "test")], check=True)
