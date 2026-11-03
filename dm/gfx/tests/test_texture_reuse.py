"""Check production packaged-texture allocation with CPU-only object mocks."""

import os
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "nvn/nvn_mtl.m").read_text()


def function(name):
    match = re.search(r"^static [^\n]+\b" + name + r"\([^\n]*\) \{", SOURCE, re.M)
    if not match:
        raise AssertionError("Missing production function " + name)
    start = SOURCE.index("{", match.start())
    depth = 0
    for end in range(start, len(SOURCE)):
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        if not depth:
            return SOURCE[match.start():end + 1]
    raise AssertionError("Unterminated production function " + name)


PREFIX = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>

static unsigned checks, failures, allocations;
#define CHECK(value) do { checks++; if (!(value)) { failures++; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #value); } } while (0)

// Only types and enum constants come from Metal. No device or Metal object is
// created, and the executable links Foundation only.
@interface MockDescriptor : NSObject
@property MTLPixelFormat pixelFormat;
@property MTLTextureType textureType;
@property NSUInteger width, height, depth, arrayLength, mipmapLevelCount, sampleCount;
@property MTLTextureUsage usage;
@end
@implementation MockDescriptor @end
@interface MockTexture : MockDescriptor @end
@implementation MockTexture @end
static NSArray<NSString *> *shape_keys;

static MockDescriptor *descriptor(void) {
    MockDescriptor *d = [MockDescriptor new];
    d.pixelFormat = MTLPixelFormatRG8Unorm; d.textureType = MTLTextureType2D;
    d.width = d.height = 256; d.depth = d.arrayLength = d.mipmapLevelCount = d.sampleCount = 1;
    d.usage = MTLTextureUsageShaderRead;
    return d;
}
static MockTexture *texture(MockDescriptor *d) {
    MockTexture *t = [MockTexture new];
    for (NSString *key in shape_keys) [t setValue:[d valueForKey:key] forKey:key];
    t.usage = d.usage;
    return t;
}
@interface MockDevice : NSObject
@property BOOL fail;
- (id<MTLTexture>)newTextureWithDescriptor:(MockDescriptor *)d;
@end
@implementation MockDevice
- (id<MTLTexture>)newTextureWithDescriptor:(MockDescriptor *)d {
    allocations++;
    return self.fail ? nil : (id)texture(d);
}
@end
static MockDevice *g_dev;
#define MTLTextureDescriptor MockDescriptor
'''

SUFFIX = r'''
int main(void) { @autoreleasepool {
    shape_keys = @[@"pixelFormat", @"textureType", @"width", @"height", @"depth",
                   @"arrayLength", @"mipmapLevelCount", @"sampleCount"];
    g_dev = [MockDevice new];
    MockDescriptor *d = descriptor();
    id<MTLTexture> first = pkg_texture_alloc(d, nil, nil);
    CHECK(first != nil && allocations == 1);
    CHECK(first.usage == MTLTextureUsageShaderRead);
    CHECK(!pkg_texture_matches(nil, d));
    CHECK(pkg_texture_matches(first, d));

    MTLTextureUsage uses[] = {MTLTextureUsageRenderTarget, MTLTextureUsagePixelFormatView,
        MTLTextureUsageShaderWrite, MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView};
    for (unsigned i = 0; i < sizeof uses / sizeof *uses; i++) {
        MockTexture *current = texture(descriptor()); current.usage |= uses[i];
        d = descriptor(); unsigned before = allocations;
        id<MTLTexture> fresh = pkg_texture_alloc(d, (id)current, nil);
        CHECK(fresh != (id)current && allocations == before + 1);
        CHECK(fresh.usage == current.usage && d.usage == current.usage);

        // A capable completed spare and a purged current texture are reused.
        before = allocations; d = descriptor();
        CHECK(pkg_texture_alloc(d, (id)current, fresh) == fresh);
        CHECK(allocations == before);
        d = descriptor(); CHECK(pkg_texture_alloc(d, (id)current, (id)current) == (id)current);
        CHECK(allocations == before);

        // Geometry alone is insufficient when the spare lacks a required use.
        MockTexture *insufficient = texture(descriptor()); d = descriptor();
        CHECK(pkg_texture_alloc(d, (id)current, (id)insufficient) != (id)insufficient);
        CHECK(allocations == before + 1);
        CHECK(insufficient.usage == MTLTextureUsageShaderRead);
    }

    // A descriptor may itself require a use absent from both prior objects.
    MockTexture *readOnly = texture(descriptor()); d = descriptor();
    d.usage |= MTLTextureUsageShaderWrite;
    unsigned before = allocations;
    id<MTLTexture> writable = pkg_texture_alloc(d, (id)readOnly, (id)readOnly);
    CHECK(writable != (id)readOnly && allocations == before + 1);
    CHECK(writable.usage == (MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite));
    MockTexture *superset = texture(d); superset.usage |= MTLTextureUsageRenderTarget;
    d = descriptor(); before = allocations;
    CHECK(pkg_texture_alloc(d, nil, (id)superset) == (id)superset);
    CHECK(allocations == before);

    for (NSString *key in shape_keys) {
        MockTexture *different = texture(descriptor());
        [different setValue:@([[different valueForKey:key] unsignedIntegerValue] + 1) forKey:key];
        different.usage |= MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView;
        d = descriptor(); CHECK(!pkg_texture_matches((id)different, d));
        before = allocations;
        id<MTLTexture> replacement = pkg_texture_alloc(d, (id)different, (id)different);
        CHECK(replacement != (id)different && allocations == before + 1);
        CHECK(replacement.usage == MTLTextureUsageShaderRead);
        CHECK(pkg_texture_matches(replacement, d));
    }

    // Exercise unchanged 3D, cube-array and multisample-array descriptors too.
    MTLTextureType types[] = {MTLTextureType3D, MTLTextureTypeCubeArray, MTLTextureType2DMultisampleArray};
    for (unsigned i = 0; i < sizeof types / sizeof *types; i++) {
        d = descriptor(); d.textureType = types[i];
        d.depth = i == 0 ? 8 : 1; d.arrayLength = i == 0 ? 1 : 3;
        d.sampleCount = i == 2 ? 4 : 1; d.mipmapLevelCount = i == 2 ? 1 : 4;
        MockTexture *current = texture(d); current.usage |= MTLTextureUsageRenderTarget;
        before = allocations;
        CHECK(pkg_texture_alloc(d, (id)current, (id)current) == (id)current);
        CHECK(allocations == before && d.usage == current.usage);
    }

    // Replacing storage repeatedly must not regress an already acquired use.
    MockTexture *current = texture(descriptor()); current.usage |= MTLTextureUsageRenderTarget;
    for (unsigned i = 0; i < 64; i++) {
        d = descriptor(); current = (id)pkg_texture_alloc(d, (id)current, nil);
        CHECK(current.usage == (MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget));
    }
    g_dev.fail = YES; d = descriptor(); before = allocations;
    CHECK(pkg_texture_alloc(d, (id)current, nil) == nil);
    CHECK(allocations == before + 1 && (current.usage & MTLTextureUsageRenderTarget));
    printf("%u checks, %u failures; no Metal device created\n", checks, failures);
    return failures ? 1 : 0;
} }
'''

with tempfile.TemporaryDirectory(prefix="nvn-texture-reuse-") as directory:
    source = Path(directory) / "test.m"
    binary = Path(directory) / "test"
    source.write_text(PREFIX + "\n" + function("pkg_texture_matches") + "\n"
                      + function("pkg_texture_alloc") + "\n" + SUFFIX)
    subprocess.run([
        "xcrun", "clang", "-x", "objective-c", "-fobjc-arc", "-O1", "-g",
        "-fsanitize=address,undefined", "-framework", "Foundation", str(source), "-o", str(binary),
    ], check=True)
    environment = os.environ.copy()
    environment["ASAN_OPTIONS"] = "detect_leaks=0"
    subprocess.run([str(binary)], env=environment, check=True)
