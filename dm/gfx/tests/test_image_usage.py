"""Exercise texture permission upgrades and shared storage with CPU-only mocks."""

import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "nvn/nvn_mtl.m").read_text()


def function(name):
    match = re.search(r"^static [^\n]+\b" + name + r"\([^\n]*\) \{", SOURCE, re.M)
    assert match, name
    start = SOURCE.index("{", match.start())
    depth = 0
    for end in range(start, len(SOURCE)):
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        if not depth:
            return SOURCE[match.start():end + 1] + "\n"
    raise AssertionError(name)


PREFIX = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <assert.h>
#include <pthread.h>
#include "nvn_hmap.h"
#include "nvn_refl.h"
#include "include/nvn.h"
static void quiet_log(const char *format, ...) { (void)format; }
#define MLOG(...) quiet_log(__VA_ARGS__)
#define RED_ONCE(...) ((void)0)
typedef struct {
    uint64_t serial;
    struct { unsigned flags, format, samples, width, height, depth, levels; int target; } d;
    int has_layout;
} tex_t;
static pthread_mutex_t g_smu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_rmu = PTHREAD_MUTEX_INITIALIZER;
static nvn_hmap g_by_serial = {.klen=8}, g_ghost = {.klen=8}, g_alias = {.klen=8};
static CFMutableSetRef g_alias_set;
static uint64_t g_ser_gen, g_pfv_upgrades;
static NSMutableArray *retired;
static unsigned copies, tracked, allocated, ended;
static int fail_allocation, pass_open;
static void (*on_end)(void);
@interface MockDescriptor : NSObject
@property MTLTextureType textureType;
@property MTLPixelFormat pixelFormat;
@property NSUInteger width, height, depth, arrayLength, mipmapLevelCount, sampleCount;
@property MTLStorageMode storageMode;
@property MTLCPUCacheMode cpuCacheMode;
@property MTLHazardTrackingMode hazardTrackingMode;
@property MTLResourceOptions resourceOptions;
@property MTLTextureUsage usage;
@end
@implementation MockDescriptor @end
#define MTLTextureDescriptor MockDescriptor
@interface MockTexture : MockDescriptor
@property(strong) NSMutableDictionary *planes;
@end
@implementation MockTexture
- (BOOL)conformsToProtocol:(Protocol *)p {
    return protocol_isEqual(p, @protocol(MTLTexture)) || [super conformsToProtocol:p];
}
@end
@interface MockDevice : NSObject
- (id)newTextureWithDescriptor:(MockDescriptor *)d;
@end
@implementation MockDevice
- (id)newTextureWithDescriptor:(MockDescriptor *)d {
    if (fail_allocation) return nil;
    MockTexture *t = [MockTexture new];
    t.textureType=d.textureType; t.pixelFormat=d.pixelFormat;
    t.width=d.width; t.height=d.height; t.depth=d.depth;
    t.arrayLength=d.arrayLength; t.mipmapLevelCount=d.mipmapLevelCount; t.sampleCount=d.sampleCount;
    t.storageMode=d.storageMode; t.cpuCacheMode=d.cpuCacheMode;
    t.hazardTrackingMode=d.hazardTrackingMode; t.resourceOptions=d.resourceOptions; t.usage=d.usage;
    t.planes=[NSMutableDictionary dictionary]; allocated++;
    return t;
}
@end
static MockDevice *g_dev;
@interface NvPkg : NSObject { @public id tex, view; unsigned vbase; } @end
@implementation NvPkg @end
@interface NvShader : NSObject { @public int stage; uint32_t image_bindings; } @end
@implementation NvShader @end
static struct { int ok; tex_t t; } g_treg[8];
static int g_ntreg=8;
static struct { uint64_t img[6][32]; } S;
static void put(nvn_hmap *map, uint64_t key, id value) {
    void *old=nvn_hm_put(map,&key,(void *)CFBridgingRetain(value));
    if (old) CFRelease(old);
}
static id get(nvn_hmap *map, uint64_t key) { return (__bridge id)nvn_hm_get(map,&key); }
static id tex_get(uint64_t key) { return get(&g_by_serial,key) ?: get(&g_ghost,key); }
static int alias_has(id value) { return CFSetContainsValue(g_alias_set,(__bridge void *)value); }
static void defer_obj_release(id value) { [retired addObject:value]; }
static void defer_obj_unref(id value) { [retired addObject:value]; }
static void res_add(id value) { assert(value); }
static void bc_blocks_share(id source, id destination) { assert(source && destination); }
static void pkg_cache_track(id object, const tex_t *source) { tracked++; }
static void rt_cache_track(id object, const tex_t *source) { tracked++; }
static void rp_end(void) {
    ended++; pass_open=0;
    if (on_end) { void (*hook)(void)=on_end; on_end=NULL; hook(); }
}
static id blit(void) { assert(!pass_open); return g_dev; }
static NSString *plane(NSUInteger slice, NSUInteger level) {
    return [NSString stringWithFormat:@"%lu:%lu",(unsigned long)slice,(unsigned long)level];
}
static void enc_b_copy_texture(id encoder, id source_, NSUInteger ss, NSUInteger sl, MTLOrigin so,
                               MTLSize size, id destination_, NSUInteger ds, NSUInteger dl, MTLOrigin d_o) {
    MockTexture *source=source_, *destination=destination_;
    assert(!pass_open && encoder && ss==ds && sl==dl);
    assert(!so.x && !so.y && !so.z && !d_o.x && !d_o.y && !d_o.z);
    assert(size.width==MAX(source.width>>sl,1u) && size.height==MAX(source.height>>sl,1u));
    assert(size.depth==(source.textureType==MTLTextureType3D ? MAX(source.depth>>sl,1u) : 1));
    NSData *bytes=source.planes[plane(ss,sl)]; assert(bytes);
    destination.planes[plane(ds,dl)]=[bytes copy]; copies++;
}
static id tex_mtl(const tex_t *source) { return tex_get(source->serial); }
'''

TEST = r'''
static tex_t color(uint64_t serial) {
    tex_t t={.serial=serial}; t.d.flags=NVN_TEXTURE_FLAGS_IMAGE|NVN_TEXTURE_FLAGS_COMPRESSIBLE;
    t.d.format=NVN_FORMAT_RGBA8; t.d.target=NVN_TEXTURE_TARGET_2D;
    return t;
}
static MockTexture *texture(MTLTextureType type) {
    MockDescriptor *d=[MockDescriptor new];
    d.textureType=type; d.pixelFormat=MTLPixelFormatRGBA8Unorm;
    d.width=8; d.height=4; d.depth=type==MTLTextureType3D ? 4 : 1;
    d.arrayLength=type==MTLTextureType2DArray ? 3 : 1;
    d.mipmapLevelCount=4; d.sampleCount=1; d.storageMode=MTLStorageModePrivate;
    d.hazardTrackingMode=MTLHazardTrackingModeTracked;
    d.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
    MockTexture *t=[g_dev newTextureWithDescriptor:d];
    for (NSUInteger s=0;s<d.arrayLength;s++) for (NSUInteger l=0;l<d.mipmapLevelCount;l++) {
        uint32_t payload[4]={(uint32_t)s,(uint32_t)l,0x12345678,0xfedcba98};
        t.planes[plane(s,l)]=[NSData dataWithBytes:payload length:sizeof payload];
    }
    return t;
}
static void register_aliases(MockTexture *t) {
    put(&g_by_serial,10,t); put(&g_by_serial,11,t); put(&g_ghost,12,t);
    put(&g_alias,100,t); put(&g_alias,101,t);
    CFSetAddValue(g_alias_set,(__bridge void *)t);
}
static void usage_tests(void) {
    tex_t t=color(10);
    int enabled=lazy_image_usage();
    assert(texture_initial_usage(&t,0)==(MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget|
                                        (enabled ? 0 : MTLTextureUsageShaderWrite)));
    unsigned excluded[]={NVN_TEXTURE_FLAGS_DISPLAY,NVN_TEXTURE_FLAGS_VIDEO_DECODE,
        NVN_TEXTURE_FLAGS_LINEAR,NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET,NVN_TEXTURE_FLAGS_SPARSE};
    for (unsigned i=0;i<sizeof excluded/sizeof *excluded;i++) {
        tex_t x=t; x.d.flags|=excluded[i]; assert(texture_initial_usage(&x,0)&MTLTextureUsageShaderWrite);
    }
    assert(texture_initial_usage(&t,1)==(MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite));
    t.d.samples=4; assert(texture_initial_usage(&t,0)&MTLTextureUsageShaderWrite); t=color(10);
    t.d.format=NVN_FORMAT_DEPTH24_STENCIL8; assert(texture_initial_usage(&t,0)&MTLTextureUsageShaderWrite);
    t=color(10); t.d.format=NVN_FORMAT_RGBA_DXT1; assert(texture_initial_usage(&t,0)&MTLTextureUsageShaderWrite);
    t=color(10); t.d.target=NVN_TEXTURE_TARGET_3D; assert(texture_initial_usage(&t,0)&MTLTextureUsageShaderWrite);
    t=color(10); t.d.flags=NVN_TEXTURE_FLAGS_COMPRESSIBLE;
    assert(texture_initial_usage(&t,0)==(MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget));
    nvn_refl reflection={0}; reflection.nb=4;
    reflection.b[0].kind=NVN_RB_IMG; reflection.b[0].reg=0;
    reflection.b[1].kind=NVN_RB_IMG; reflection.b[1].reg=31;
    reflection.b[2].kind=NVN_RB_IMG; reflection.b[2].reg=32;
    reflection.b[3].kind=NVN_RB_TEX; reflection.b[3].reg=2;
    assert(shader_image_bindings(&reflection)==0x80000001u);
}
static void upgrade_tests(void) {
    tex_t source=color(10); MockTexture *original=texture(MTLTextureType2DArray);
    register_aliases(original); NSObject *other=[NSObject new]; put(&g_by_serial,20,other);
    uint64_t generation=g_ser_gen; unsigned before=copies, retirement=(unsigned)retired.count;
    MockTexture *replacement=(id)tex_upgrade(&source,(id)original,MTLTextureUsageShaderWrite,original.pixelFormat);
    assert(replacement!=original && copies-before==12 && retired.count==retirement+1);
    assert([replacement.planes isEqual:original.planes]);
    assert(replacement.usage==(original.usage|MTLTextureUsageShaderWrite));
    assert(replacement.hazardTrackingMode==original.hazardTrackingMode);
    assert(get(&g_by_serial,10)==replacement && get(&g_by_serial,11)==replacement);
    assert(get(&g_ghost,12)==replacement && get(&g_alias,100)==replacement && get(&g_alias,101)==replacement);
    assert(!alias_has(original) && alias_has(replacement) && get(&g_by_serial,20)==other);
    assert(g_ser_gen==generation+1 && [retired containsObject:original]);
    fail_allocation=1; before=copies; generation=g_ser_gen;
    assert(tex_upgrade(&source,(id)replacement,MTLTextureUsagePixelFormatView,replacement.pixelFormat)==(id)replacement);
    assert(copies==before && g_ser_gen==generation && get(&g_by_serial,10)==replacement);
    fail_allocation=0;
    MockTexture *volume=texture(MTLTextureType3D); source=color(30); put(&g_by_serial,30,volume);
    before=copies;
    MockTexture *volume_copy=(id)tex_upgrade(&source,(id)volume,MTLTextureUsageShaderWrite,volume.pixelFormat);
    assert(copies-before==4 && [volume.planes isEqual:volume_copy.planes]);
}
static void change_registration(void) {
    assert(pthread_mutex_trylock(&g_rmu)==0);
    g_treg[3].t=color(40);
    pthread_mutex_unlock(&g_rmu);
}
static void preparation_tests(void) {
    if (!lazy_image_usage()) return;
    MockTexture *t=texture(MTLTextureType2DArray); register_aliases(t);
    g_treg[3].ok=1; g_treg[3].t=color(10);
    NvShader *shader=[NvShader new]; shader->stage=5; shader->image_bindings=1u<<31;
    S.img[5][31]=3; pass_open=1; unsigned before=copies, old_ended=ended;
    image_usage_prepare_shader(shader);
    assert(!pass_open && ended==old_ended+1 && copies-before==12);
    assert(get(&g_by_serial,10)!=t && get(&g_by_serial,10)==get(&g_ghost,12));
    assert(((MockTexture *)get(&g_by_serial,10)).usage&MTLTextureUsageShaderWrite);
    pass_open=1; before=copies; old_ended=ended;
    image_usage_prepare_shader(shader);
    assert(pass_open && ended==old_ended && copies==before);
    MockTexture *fresh=texture(MTLTextureType2D); fresh.usage|=MTLTextureUsageShaderWrite;
    put(&g_by_serial,40,fresh); register_aliases(t); on_end=change_registration;
    image_usage_prepare_shader(shader);
    assert(!pass_open && copies==before && get(&g_by_serial,10)==t && get(&g_by_serial,40)==fresh);
    shader->stage=6; pass_open=1; image_usage_prepare_shader(shader); assert(pass_open);
    shader->stage=5; S.img[5][31]=100; image_usage_prepare_shader(shader); assert(pass_open);
    pass_open=0;
}
static void release_value(void *value) { CFRelease(value); }
int main(void) { @autoreleasepool {
    g_dev=[MockDevice new]; retired=[NSMutableArray array];
    g_alias_set=CFSetCreateMutable(NULL,0,NULL);
    usage_tests(); upgrade_tests(); preparation_tests();
    nvn_hm_clear(&g_by_serial,release_value); nvn_hm_clear(&g_ghost,release_value);
    nvn_hm_clear(&g_alias,release_value); CFRelease(g_alias_set);
    free(g_by_serial.keys); free(g_by_serial.vals); free(g_ghost.keys); free(g_ghost.vals);
    free(g_alias.keys); free(g_alias.vals);
    [retired removeAllObjects];
    printf("CPU image usage checks passed: lazy=%d, copies=%u, upgrades=%llu\n",
           lazy_image_usage(),copies,(unsigned long long)g_pfv_upgrades);
} }
'''

names = ("fmt_depth", "fmt_compressed", "lazy_image_usage", "texture_defer_image_usage",
         "texture_initial_usage", "texture_replace_references", "tex_upgrade",
         "shader_image_bindings", "image_usage_prepare_shader")
with tempfile.TemporaryDirectory(prefix="nvn-image-usage-") as directory:
    source = Path(directory) / "test.m"
    binary = Path(directory) / "test"
    source.write_text(PREFIX + "\n".join(function(name) for name in names) + TEST)
    subprocess.run(["clang", "-g", "-O1", "-fobjc-arc", "-fsanitize=address,undefined",
                    "-I" + str(ROOT / "nvn"), str(source), "-framework", "Foundation",
                    "-o", str(binary)], check=True)
    for enabled in ("1", "0"):
        environment = dict(os.environ, DM_NVN_LAZY_IMAGE_USAGE=enabled,
                           ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="halt_on_error=1")
        subprocess.run([str(binary)], check=True, env=environment)
