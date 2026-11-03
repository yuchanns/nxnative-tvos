"""Compare production argument batching with scalar encoding on CPU mocks."""

import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "nvn_refl.h"
#include <stdio.h>

static unsigned checks, failures;
#define CHECK(value) do { checks++; if (!(value)) { failures++; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #value); } } while (0)
#define MDIE(...) @throw [NSException exceptionWithName:@"InvalidEncoding" \
    reason:[NSString stringWithFormat:@__VA_ARGS__] userInfo:nil]

// Metal contributes types only. All allocated objects are Foundation mocks;
// this executable never creates a device or links the Metal framework.
@interface MockBuffer : NSObject
@property NSMutableData *data;
@property(readonly) NSUInteger length;
@property(readonly) void *contents;
@end
@implementation MockBuffer
- (instancetype)init { if ((self = [super init])) _data = [NSMutableData dataWithLength:4096]; return self; }
- (NSUInteger)length { return self.data.length; }
- (void *)contents { return self.data.mutableBytes; }
@end

static NSArray *owners;
static id<MTLBuffer> gpu_buffer_required(uint64_t token, uint64_t bytes, NSUInteger *offset) {
    CHECK(bytes == 1 && token >= 4096 && token < 4 * 4096);
    *offset = token % 4096;
    return (id)owners[token / 4096 - 1];
}

@interface MockEncoder : NSObject
@property NSUInteger encodedLength, alignment, destinationOffset, apiCalls, largestBatch;
@property MockBuffer *destination;
@property NSMutableArray *events;
- (void)setArgumentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset;
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index;
- (void)setTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index;
- (void)setSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index;
- (void)setBuffers:(id<MTLBuffer> const __unsafe_unretained *)buffers offsets:(const NSUInteger *)offsets withRange:(NSRange)range;
- (void)setTextures:(id<MTLTexture> const __unsafe_unretained *)textures withRange:(NSRange)range;
- (void)setSamplerStates:(id<MTLSamplerState> const __unsafe_unretained *)samplers withRange:(NSRange)range;
@end
@implementation MockEncoder
- (instancetype)init {
    if ((self = [super init])) { _encodedLength = 192 * 16; _alignment = 256; _events = [NSMutableArray new]; }
    return self;
}
- (void)setArgumentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset {
    self.destination = (id)buffer; self.destinationOffset = offset;
}
- (void)record:(unsigned)kind resource:(id)resource offset:(NSUInteger)offset index:(NSUInteger)index {
    CHECK(index < NVN_REFL_MAXARG);
    uint64_t pointer = (uintptr_t)(__bridge void *)resource;
    [self.events addObject:@[@(kind), @(index), @(pointer), @(offset)]];
    uint64_t values[2] = {pointer ^ kind, offset};
    memcpy((uint8_t *)self.destination.contents + self.destinationOffset + index * 16, values, sizeof values);
}
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    self.apiCalls++; [self record:1 resource:buffer offset:offset index:index];
}
- (void)setTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index {
    self.apiCalls++; [self record:2 resource:texture offset:0 index:index];
}
- (void)setSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index {
    self.apiCalls++; [self record:3 resource:sampler offset:0 index:index];
}
- (void)setBuffers:(id<MTLBuffer> const __unsafe_unretained *)buffers offsets:(const NSUInteger *)offsets withRange:(NSRange)range {
    self.apiCalls++; self.largestBatch = MAX(self.largestBatch, range.length);
    for (NSUInteger i = 0; i < range.length; i++) [self record:1 resource:buffers[i] offset:offsets[i] index:range.location + i];
}
- (void)setTextures:(id<MTLTexture> const __unsafe_unretained *)textures withRange:(NSRange)range {
    self.apiCalls++; self.largestBatch = MAX(self.largestBatch, range.length);
    for (NSUInteger i = 0; i < range.length; i++) [self record:2 resource:textures[i] offset:0 index:range.location + i];
}
- (void)setSamplerStates:(id<MTLSamplerState> const __unsafe_unretained *)samplers withRange:(NSRange)range {
    self.apiCalls++; self.largestBatch = MAX(self.largestBatch, range.length);
    for (NSUInteger i = 0; i < range.length; i++) [self record:3 resource:samplers[i] offset:0 index:range.location + i];
}
@end

#include "nvn_argument_encoding.h"

// The original scalar path is the oracle, independent of batch construction.
static void scalar_encode(id<MTLArgumentEncoder> encoder, id<MTLBuffer> destination,
                          NSUInteger offset, const nvn_refl *reflection, const uint64_t *slots,
                          int compact_samplers) {
    memset((uint8_t *)destination.contents + offset, 0, encoder.encodedLength);
    [encoder setArgumentBuffer:destination offset:offset];
    for (int i = 0; i < reflection->nb; i++) {
        const nvn_rbind *binding = &reflection->b[i];
        if (compact_samplers && argument_is_sampler(binding->kind)) continue;
        unsigned count = binding->n ? binding->n : 1;
        for (unsigned element = 0; element < count; element++) {
            NSUInteger index = binding->aid + element;
            uint64_t token = slots[index];
            switch (binding->kind) {
            case NVN_RB_CBUF: case NVN_RB_SBUFP: case NVN_RB_SBUFU: case NVN_RB_UBOP:
            case NVN_RB_SUPPORT: case NVN_RB_C1: case NVN_RB_DRV0: case NVN_RB_DRV2: case NVN_RB_IMGA: {
                NSUInteger buffer_offset = 0;
                id<MTLBuffer> buffer = token ? gpu_buffer_required(token, 1, &buffer_offset) : nil;
                [encoder setBuffer:buffer offset:buffer_offset atIndex:index]; break;
            }
            case NVN_RB_TEX: case NVN_RB_TEXU: case NVN_RB_IMG: case NVN_RB_TBUF: case NVN_RB_TEXD:
                [encoder setTexture:(__bridge id<MTLTexture>)(void *)(uintptr_t)token atIndex:index]; break;
            case NVN_RB_SAMP: case NVN_RB_SAMPT: case NVN_RB_SAMPU: case NVN_RB_SAMPI: case NVN_RB_SAMPD:
                [encoder setSamplerState:(__bridge id<MTLSamplerState>)(void *)(uintptr_t)token atIndex:index]; break;
            default: abort();
            }
        }
    }
}

static int is_buffer(unsigned kind) {
    const unsigned kinds[] = {NVN_RB_CBUF, NVN_RB_SBUFP, NVN_RB_SBUFU, NVN_RB_UBOP,
        NVN_RB_SUPPORT, NVN_RB_C1, NVN_RB_DRV0, NVN_RB_DRV2, NVN_RB_IMGA};
    for (unsigned i = 0; i < sizeof kinds / sizeof *kinds; i++) if (kind == kinds[i]) return 1;
    return 0;
}

static NSUInteger compare(const nvn_refl *reflection, int compact) {
    uint64_t slots[NVN_REFL_MAXARG] = {0};
    for (int i = 0; i < reflection->nb; i++) {
        nvn_rbind binding = reflection->b[i];
        for (unsigned j = 0; j < (binding.n ? binding.n : 1); j++) {
            unsigned index = binding.aid + j;
            slots[index] = index % 5 == 0 ? 0 : is_buffer(binding.kind)
                ? (1 + index % 3) * 4096 + index * 7
                : (uintptr_t)(__bridge void *)owners[index % 3];
        }
    }
    MockBuffer *old = [MockBuffer new], *now = [MockBuffer new];
    memset(old.contents, 0xa5, old.length); memset(now.contents, 0xa5, now.length);
    MockEncoder *scalar = [MockEncoder new], *batch = [MockEncoder new];
    scalar_encode((id)scalar, (id)old, 256, reflection, slots, compact);
    argument_encode((id)batch, (id)now, 256, reflection, slots, compact);
    CHECK([scalar.events isEqual:batch.events]);
    CHECK([old.data isEqual:now.data]);
    CHECK(batch.apiCalls <= scalar.apiCalls);
    CHECK(batch.largestBatch <= ARGUMENT_BATCH_MAX);
    return batch.apiCalls;
}

int main(void) { @autoreleasepool {
    owners = @[[NSObject new], [NSObject new], [NSObject new]];
    nvn_refl r = {0}; CHECK(compare(&r, 0) == 0);
    for (unsigned kind = 1; kind <= NVN_RB_IMGA; kind++) {
        r = (nvn_refl){.nb = 1, .b = {{.kind = kind, .aid = 0, .n = 192}}};
        CHECK(compare(&r, 0) == 6); compare(&r, 1);
        r.b[0].n = 0; CHECK(compare(&r, 0) == 1);
        r.b[0].aid = 191; compare(&r, 0);
    }
    // Adjacent scalar declarations can batch; holes and type changes cannot.
    r = (nvn_refl){.nb = 3, .b = {{.kind=NVN_RB_CBUF, .aid=3},
        {.kind=NVN_RB_C1, .aid=4}, {.kind=NVN_RB_SUPPORT, .aid=5}}};
    CHECK(compare(&r, 0) == 1);
    r.b[1].aid = 8; CHECK(compare(&r, 0) == 3);
    r.b[1].aid = 4; r.b[1].kind = NVN_RB_TEX; CHECK(compare(&r, 0) == 3);
    r = (nvn_refl){.nb=3, .b={{.kind=NVN_RB_TEXD,.aid=0,.n=33},
        {.kind=NVN_RB_TEXD,.aid=33,.n=31}, {.kind=NVN_RB_TEX,.aid=64}}};
    CHECK(compare(&r, 0) == 3);
    r.b[1] = (nvn_rbind){.kind=NVN_RB_SAMPD,.aid=33,.n=31};
    CHECK(compare(&r, 1) == 3);
    // Repeated and descending writes retain reflection order and last-write semantics.
    r = (nvn_refl){.nb=3,.b={{.kind=NVN_RB_TEX,.aid=8},
        {.kind=NVN_RB_TEX,.aid=8}, {.kind=NVN_RB_TEX,.aid=7}}};
    CHECK(compare(&r, 0) == 3);
    uint32_t random = 0x6353;
    for (unsigned test = 0; test < 500; test++) {
        r = (nvn_refl){0}; unsigned next = test % 7;
        while (r.nb < 32 && next < NVN_REFL_MAXARG) {
            random = random * 1664525 + 1013904223;
            unsigned kind = 1 + random % NVN_RB_IMGA, count = MIN(1 + (random >> 8) % 19, NVN_REFL_MAXARG - next);
            r.b[r.nb++] = (nvn_rbind){.kind=kind,.aid=next,.n=count};
            next += count + ((random >> 16) % 3);
        }
        compare(&r, test & 1);
    }
    // The compact table still initializes every physical slot, including padding.
    for (unsigned used = 1; used <= 16; used++) {
        uint64_t physical[16];
        for (unsigned i = 0; i < 16; i++) physical[i] = (uintptr_t)(__bridge void *)owners[i % 3];
        MockEncoder *encoder = [MockEncoder new]; MockBuffer *buffer = [MockBuffer new];
        [encoder setArgumentBuffer:(id)buffer offset:0];
        argument_sampler_table((id)encoder, 20, physical, used);
        CHECK(encoder.apiCalls == 1 && encoder.events.count == 16);
        for (unsigned i = 0; i < 16; i++) CHECK(([encoder.events[i] isEqual:@[@3,@(20+i),@(physical[i < used ? i : 0]),@0]]));
    }
    for (unsigned invalid = 0; invalid < 7; invalid++) {
        MockEncoder *encoder = [MockEncoder new]; MockBuffer *buffer = [MockBuffer new];
        uint64_t slots[NVN_REFL_MAXARG] = {0}; r = (nvn_refl){.nb=1,.b={{.kind=NVN_RB_TEX}}};
        NSUInteger offset = 256;
        if (invalid == 1) offset = 257;
        if (invalid == 2) offset = 4097;
        if (invalid == 3) offset = 2048;
        if (invalid == 4) r.b[0].aid = 192;
        if (invalid == 5) { r.b[0].aid = 191; r.b[0].n = 2; }
        if (invalid == 6) r.b[0].kind = 255;
        BOOL caught = NO;
        @try { argument_encode(invalid == 0 ? nil : (id)encoder, (id)buffer, offset, &r, slots, 0); }
        @catch (NSException *e) { caught = [e.name isEqual:@"InvalidEncoding"]; }
        CHECK(caught);
    }
    printf("%u checks, %u failures; scalar equivalence, no Metal device created\n", checks, failures);
    return failures ? 1 : 0;
} }
'''

with tempfile.TemporaryDirectory(prefix="nvn-argument-batch-") as directory:
    source = Path(directory) / "test.m"
    binary = Path(directory) / "test"
    source.write_text(SOURCE)
    subprocess.run([
        "xcrun", "clang", "-x", "objective-c", "-fobjc-arc", "-O1", "-g",
        "-fsanitize=address,undefined", "-framework", "Foundation",
        "-I", str(ROOT / "nvn"), str(source), "-o", str(binary),
    ], check=True)
    environment = os.environ.copy()
    environment["ASAN_OPTIONS"] = "detect_leaks=0"
    subprocess.run([str(binary)], env=environment, check=True)
