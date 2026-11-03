// Opt-in diagnostics for remote tvOS testing. Only fixed diagnostic files and
// files beneath the game's save directory are served; no command execution.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <GameController/GameController.h>
#import <UIKit/UIKit.h>
#import <AVFAudio/AVFAudio.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach/mach.h>
#include <os/proc.h>
#include <os/lock.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dm_input.h"
#include "dm.h"
#include "dmsh_chain.h"
#include "hle_audio_diag.h"
#import "cloud_save.h"
#import "listener.h"

void dm_gfx_shot_at(long frames, const char *path);
int mtl_passgpu_request(unsigned frames);
uint64_t mtl_vertex_overlap_request(int enabled);
NSDictionary *mtl_vertex_overlap_status(void);
uint64_t mtl_vertex_buffers_request(int enabled);
NSDictionary *mtl_vertex_buffers_status(void);
uint64_t mtl_depth_attachments_request(int enabled);
NSDictionary *mtl_depth_attachments_status(void);
NSDictionary *dm_tvos_asset_status(void);
NSDictionary *mtl_memory_snapshot(void);

static os_unfair_lock g_ui_lock = OS_UNFAIR_LOCK_INIT;
static NSDictionary *g_ui_snapshot;
static os_unfair_lock g_gpu_lock = OS_UNFAIR_LOCK_INIT;
static NSDictionary *g_gpu_snapshot;
static dispatch_queue_t g_trace_queue;
static dispatch_source_t g_memory_timer, g_ui_timer, g_pressure_source, g_gpu_timer;
static NSMutableArray *g_lifecycle_observers;

static NSData *json_data(id object) {
    return [NSJSONSerialization dataWithJSONObject:object options:0 error:NULL];
}
static NSDictionary *system_memory_status(void) {
    vm_statistics64_data_t vm = {0};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    mach_port_t host = mach_host_self();
    kern_return_t result = host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&vm, &count);
    mach_port_deallocate(mach_task_self(), host);
    if (result != KERN_SUCCESS) return @{@"error": @(result)};
    uint64_t page = vm_page_size;
    return @{@"free_bytes": @(page * vm.free_count), @"speculative_bytes": @(page * vm.speculative_count),
        @"active_bytes": @(page * vm.active_count), @"inactive_bytes": @(page * vm.inactive_count),
        @"wired_bytes": @(page * vm.wire_count), @"compressor_bytes": @(page * vm.compressor_page_count),
        @"compressed_original_bytes": @(page * vm.total_uncompressed_pages_in_compressor),
        @"pageins": @(vm.pageins), @"pageouts": @(vm.pageouts),
        @"compressions": @(vm.compressions), @"decompressions": @(vm.decompressions)};
}
static NSDictionary *ui_status(void) {
    os_unfair_lock_lock(&g_ui_lock);
    NSDictionary *snapshot = g_ui_snapshot;
    os_unfair_lock_unlock(&g_ui_lock);
    return snapshot ?: @{@"pending_main_thread_sample": @YES};
}
static NSDictionary *gpu_status(void) {
    os_unfair_lock_lock(&g_gpu_lock);
    NSDictionary *snapshot = g_gpu_snapshot;
    os_unfair_lock_unlock(&g_gpu_lock);
    return snapshot ?: @{@"pending_inventory_sample": @YES};
}
static NSArray *audio_status(void) {
    dm_audio_diagnostic samples[8];
    size_t count = dm_audio_snapshot(samples, sizeof samples / sizeof *samples);
    NSMutableArray *outputs = [NSMutableArray arrayWithCapacity:count];
    for (size_t i = 0; i < count; i++) {
        dm_audio_diagnostic *d = samples + i;
        double queue_ms = d->channels > 0 ? d->queued_bytes * 1000.0 / (48000.0 * 2 * d->channels) : 0;
        uint64_t age = d->oldest_enqueue_ns && d->callback_ns >= d->oldest_enqueue_ns ? d->callback_ns - d->oldest_enqueue_ns : 0;
        [outputs addObject:@{@"id": @(d->id), @"channels": @(d->channels), @"state": @(d->state),
            @"backend": @(d->backend), @"callback_ns": @(d->callback_ns), @"callbacks": @(d->callbacks),
            @"queued_ms": @(queue_ms), @"oldest_block_age_ms": @(age / 1e6),
            @"host_time_valid": @(d->host_time_valid), @"callback_lead_ms": @(d->callback_lead_ns / 1e6)}];
    }
    return outputs;
}
static NSDictionary *memory_status(void) {
    task_vm_info_data_t vm = {0}; mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    kern_return_t result = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &count);
    size_t headroom = os_proc_available_memory();
    return @{@"task_info_error": @(result), @"footprint_bytes": @(vm.phys_footprint),
        @"internal_bytes": @(vm.internal), @"compressed_bytes": @(vm.compressed),
        @"resident_bytes": @(vm.resident_size), @"reusable_bytes": @(vm.reusable),
        @"graphics_footprint_bytes": @(vm.ledger_tag_graphics_footprint),
        @"graphics_compressed_bytes": @(vm.ledger_tag_graphics_footprint_compressed),
        @"process_headroom_bytes": @(headroom), @"available_memory_bytes": @(headroom),
        @"physical_memory_bytes": @(NSProcessInfo.processInfo.physicalMemory), @"vm_page_size_bytes": @(vm_page_size),
        @"system_memory": system_memory_status(), @"system_uptime_seconds": @(NSProcessInfo.processInfo.systemUptime)};
}
// Only the serial trace queue writes these files. Keep the previous bounded file
// across rotation so a long session cannot consume the game's asset storage.
static void trace_append(NSString *directory, NSString *name, NSDictionary *record, off_t limit) {
    NSData *json = json_data(record);
    if (!json) return;
    NSMutableData *line = [json mutableCopy]; [line appendBytes:"\n" length:1];
    NSString *path = [directory stringByAppendingPathComponent:name];
    struct stat st;
    if (!stat(path.fileSystemRepresentation, &st) && st.st_size + (off_t)line.length > limit) {
        NSString *previous = [path stringByAppendingString:@".previous"];
        if (rename(path.fileSystemRepresentation, previous.fileSystemRepresentation)) return;
    }
    int fd = open(path.fileSystemRepresentation, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return;
    const uint8_t *bytes = line.bytes; size_t offset = 0;
    while (offset < line.length) {
        ssize_t written = write(fd, bytes + offset, line.length - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) break;
        offset += (size_t)written;
    }
    close(fd);
}
static NSMutableDictionary *trace_record(NSString *event) {
    return [@{@"event": event, @"pid": @(getpid()), @"clock_ns": @(now_ns()),
        @"wall_time_ms": @((uint64_t)(NSDate.date.timeIntervalSince1970 * 1000)),
        @"system_uptime_seconds": @(NSProcessInfo.processInfo.systemUptime)} mutableCopy];
}
// UIKit is read only on its main queue. HTTP and memory sampling use this cached
// snapshot and its timestamp, so a stalled main thread cannot block diagnostics.
static void sample_ui(NSString *directory, NSString *event) {
    UIApplication *app = UIApplication.sharedApplication;
    NSMutableArray *scenes = [NSMutableArray array];
    for (UIScene *scene in app.connectedScenes) [scenes addObject:@(scene.activationState)];
    NSMutableDictionary *record = trace_record(event);
    record[@"application_state"] = @(app.applicationState);
    record[@"idle_timer_disabled"] = @(app.idleTimerDisabled);
    record[@"scene_activation_states"] = scenes;
    AVAudioSession *audio = AVAudioSession.sharedInstance;
    NSMutableArray *ports = [NSMutableArray array];
    for (AVAudioSessionPortDescription *port in audio.currentRoute.outputs)
        [ports addObject:@{@"type": port.portType, @"channels": @(port.channels.count)}];
    record[@"audio_session"] = @{@"sample_rate": @(audio.sampleRate),
        @"io_buffer_ms": @(audio.IOBufferDuration * 1000),
        @"output_latency_ms": @(audio.outputLatency * 1000), @"outputs": ports};
    NSDictionary *snapshot = [record copy];
    os_unfair_lock_lock(&g_ui_lock); g_ui_snapshot = snapshot; os_unfair_lock_unlock(&g_ui_lock);
    dispatch_async(g_trace_queue, ^{ trace_append(directory, @"lifecycle.jsonl", snapshot, 256 << 10); });
}
static void diagnostics_recording_start(NSString *directory) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        g_trace_queue = dispatch_queue_create("dm.tv.diagnostic-recording", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
        dispatch_queue_t gpu_queue = dispatch_queue_create("dm.tv.gpu-inventory", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_BACKGROUND, 0));
        g_gpu_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, gpu_queue);
        dispatch_source_set_timer(g_gpu_timer, DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC, NSEC_PER_SEC);
        dispatch_source_set_event_handler(g_gpu_timer, ^{ @autoreleasepool {
            static uint64_t retry_after;
            if (now_ns() < retry_after) return;
            NSDictionary *snapshot = mtl_memory_snapshot();
            os_unfair_lock_lock(&g_gpu_lock); g_gpu_snapshot = snapshot; os_unfair_lock_unlock(&g_gpu_lock);
            // Metadata queries can fault under physical memory pressure. Back
            // off without blocking the five-second memory/lifecycle recorder.
            if ([snapshot[@"snapshot_duration_ns"] unsignedLongLongValue] > NSEC_PER_SEC / 4)
                retry_after = now_ns() + 120 * NSEC_PER_SEC;
        } });
        dispatch_resume(g_gpu_timer);
        dispatch_async(dispatch_get_main_queue(), ^{
            g_lifecycle_observers = [NSMutableArray array];
            for (NSString *name in @[UIApplicationDidBecomeActiveNotification, UIApplicationWillResignActiveNotification,
                UIApplicationDidEnterBackgroundNotification, UIApplicationWillEnterForegroundNotification,
                UIApplicationWillTerminateNotification, UIApplicationDidReceiveMemoryWarningNotification,
                UISceneDidActivateNotification, UISceneWillDeactivateNotification,
                UISceneDidEnterBackgroundNotification, UISceneWillEnterForegroundNotification]) {
                id observer = [NSNotificationCenter.defaultCenter addObserverForName:name object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) {
                    sample_ui(directory, note.name);
                }];
                [g_lifecycle_observers addObject:observer];
            }
            sample_ui(directory, @"start");
            g_ui_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
            dispatch_source_set_timer(g_ui_timer, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), 5 * NSEC_PER_SEC, NSEC_PER_SEC / 2);
            dispatch_source_set_event_handler(g_ui_timer, ^{ sample_ui(directory, @"heartbeat"); });
            dispatch_resume(g_ui_timer);
        });
        g_memory_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, g_trace_queue);
        dispatch_source_set_timer(g_memory_timer, DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC, NSEC_PER_SEC / 2);
        dispatch_source_set_event_handler(g_memory_timer, ^{ @autoreleasepool {
            static unsigned samples;
            NSMutableDictionary *record = trace_record(@"sample");
            [record addEntriesFromDictionary:memory_status()];
            record[@"ui"] = ui_status();
            record[@"audio_outputs"] = audio_status();
            record[@"metal_allocated_bytes"] = @(MTLCreateSystemDefaultDevice().currentAllocatedSize);
            if (samples++ % 6 == 0) record[@"gpu_resources"] = gpu_status();
            trace_append(directory, @"runtime-memory.jsonl", record, 2 << 20);
        } });
        dispatch_resume(g_memory_timer);
        g_pressure_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
            DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL, g_trace_queue);
        dispatch_source_set_event_handler(g_pressure_source, ^{ @autoreleasepool {
            NSMutableDictionary *record = trace_record(@"memory_pressure");
            record[@"pressure_flags"] = @(dispatch_source_get_data(g_pressure_source));
            [record addEntriesFromDictionary:memory_status()];
            trace_append(directory, @"runtime-memory.jsonl", record, 2 << 20);
        } });
        dispatch_resume(g_pressure_source);
    });
}
static NSData *file_tail(NSString *path, size_t limit) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY);
    if (fd < 0) return nil;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return nil; }
    size_t count = MIN((uint64_t)st.st_size, limit);
    NSMutableData *data = [NSMutableData dataWithLength:count];
    ssize_t got = pread(fd, data.mutableBytes, count, st.st_size - count);
    close(fd);
    if (got < 0) return nil;
    data.length = (NSUInteger)got;
    return data;
}
static NSData *status_data(NSString *directory) {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    NSDictionary *space = [NSFileManager.defaultManager attributesOfFileSystemForPath:directory error:NULL];
    NSMutableArray *controllers = [NSMutableArray array];
    for (GCController *controller in GCController.controllers)
        [controllers addObject:@{@"name": controller.vendorName ?: @"unknown", @"extended": @(controller.extendedGamepad != nil)}];
    NSMutableArray *modules = [NSMutableArray array];
    for (int i = 0; i < g_nmod; i++)
        [modules addObject:@{@"name": @(g_mod[i].name), @"base": @((uintptr_t)g_mod[i].base),
            @"size": @(g_mod[i].size), @"build_id": @(g_mod[i].build_id)}];
    NSMutableDictionary *status = [memory_status() mutableCopy];
    [status addEntriesFromDictionary:@{@"pid": @(getpid()), @"chain": @DMSH_CHAIN,
        @"clock_ns": @(now_ns()), @"process_start_ns": @(g_t0_ns), @"modules": modules,
        @"metal_allocated_bytes": @(device.currentAllocatedSize), @"input_samples": @(dm_input_samples()),
        @"disk_free_bytes": space[NSFileSystemFreeSize] ?: @0, @"controllers": controllers, @"ui": ui_status(),
        @"audio_outputs": audio_status()}];
    return json_data(status);
}
static int send_all(int connection, NSData *data) {
    const uint8_t *bytes = data.bytes; size_t offset = 0;
    while (offset < data.length) {
        ssize_t count = send(connection, bytes + offset, data.length - offset, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return 0;
        offset += (size_t)count;
    }
    return 1;
}
static NSData *save_file(NSString *directory, NSString *name) {
    if (!name.length) return nil;
    NSString *root = [directory stringByAppendingPathComponent:@"save"];
    NSString *candidate = [root stringByAppendingPathComponent:name];
    char resolved[PATH_MAX], root_path[PATH_MAX];
    if (!realpath(root.fileSystemRepresentation, root_path) || !realpath(candidate.fileSystemRepresentation, resolved)) return nil;
    size_t length = strlen(root_path);
    if (strncmp(root_path, resolved, length) || resolved[length] != '/') return nil;
    struct stat st;
    if (stat(resolved, &st) || !S_ISREG(st.st_mode) || st.st_size > (16 << 20)) return nil;
    return [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:resolved] options:NSDataReadingMappedIfSafe error:NULL];
}
void dm_tvos_diagnostics_start(const char *path) {
    const char *port_value = getenv("DM_TV_DIAGNOSTIC_PORT"), *token_value = getenv("DM_TV_DIAGNOSTIC_TOKEN");
    int port = port_value ? atoi(port_value) : 0;
    if (port < 1 || port > 65535 || !token_value || strlen(token_value) < 16) return;
    NSString *directory = [NSString stringWithUTF8String:path];
    NSString *authorization = [NSString stringWithFormat:@"Authorization: Bearer %s", token_value];
    diagnostics_recording_start(directory);
    // Preserve serial request processing while allowing the next request to wait
    // for the previous connection's completion on the listener queue.
    dispatch_queue_t requests = dispatch_queue_create("dm.tv.diagnostic-http",
        dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    dm_tvos_listener_start("diagnostics", port, 4, requests, ^(int connection) {
        int yes = 1;
        if (setsockopt(connection, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes)) return;
        struct timeval timeout = {.tv_sec = 5};
        setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
        char request[8192] = {0}; size_t used = 0;
        while (used < sizeof request - 1 && !strstr(request, "\r\n\r\n")) {
            ssize_t got = recv(connection, request + used, sizeof request - 1 - used, 0);
            if (got <= 0) break;
            used += (size_t)got;
        }
        NSArray<NSString *> *lines = [[NSString stringWithUTF8String:request] componentsSeparatedByString:@"\r\n"];
        NSArray<NSString *> *first = [lines.firstObject componentsSeparatedByString:@" "];
        int code = 404; NSData *body = nil; NSString *type = @"application/octet-stream";
        if (![lines containsObject:authorization]) code = 403;
        else if (first.count == 3) {
            NSString *method = first[0], *route = first[1];
            if ([method isEqualToString:@"POST"] && [route isEqualToString:@"/screenshot"]) {
                dm_gfx_shot_at(1, [directory stringByAppendingPathComponent:@"latest.png"].fileSystemRepresentation);
                body = json_data(@{@"queued": @YES}); code = 202;
            } else if ([method isEqualToString:@"POST"] && [route isEqualToString:@"/gpu-capture"]) {
                BOOL queued = mtl_passgpu_request(300);
                body = json_data(@{@"queued": @(queued), @"frames": @300});
                type = @"application/json"; code = queued ? 202 : 409;
            } else if ([method isEqualToString:@"POST"] &&
                       ([route isEqualToString:@"/vertex-overlap/0"] || [route isEqualToString:@"/vertex-overlap/1"])) {
                int enabled = [route hasSuffix:@"/1"];
                body = json_data(@{@"queued": @YES, @"enabled": @(enabled),
                                   @"generation": @(mtl_vertex_overlap_request(enabled))});
                type = @"application/json"; code = 202;
            } else if ([method isEqualToString:@"POST"] &&
                       ([route isEqualToString:@"/vertex-buffers/0"] || [route isEqualToString:@"/vertex-buffers/1"])) {
                int enabled = [route hasSuffix:@"/1"];
                body = json_data(@{@"queued": @YES, @"enabled": @(enabled),
                                   @"generation": @(mtl_vertex_buffers_request(enabled))});
                type = @"application/json"; code = 202;
            } else if ([method isEqualToString:@"POST"] &&
                       ([route isEqualToString:@"/depth-attachments/0"] || [route isEqualToString:@"/depth-attachments/1"])) {
                int enabled = [route hasSuffix:@"/1"];
                body = json_data(@{@"queued": @YES, @"enabled": @(enabled),
                                   @"generation": @(mtl_depth_attachments_request(enabled))});
                type = @"application/json"; code = 202;
            } else if ([method isEqualToString:@"GET"]) {
                if ([route isEqualToString:@"/status"]) { body = status_data(directory); type = @"application/json"; }
                else if ([route isEqualToString:@"/gpu-memory"]) { body = json_data(gpu_status()); type = @"application/json"; }
                else if ([route isEqualToString:@"/vertex-overlap"]) { body = json_data(mtl_vertex_overlap_status()); type = @"application/json"; }
                else if ([route isEqualToString:@"/vertex-buffers"]) { body = json_data(mtl_vertex_buffers_status()); type = @"application/json"; }
                else if ([route isEqualToString:@"/depth-attachments"]) { body = json_data(mtl_depth_attachments_status()); type = @"application/json"; }
                else if ([route isEqualToString:@"/lifecycle"]) body = file_tail([directory stringByAppendingPathComponent:@"lifecycle.jsonl"], 256 << 10);
                else if ([route isEqualToString:@"/runtime-memory"]) body = file_tail([directory stringByAppendingPathComponent:@"runtime-memory.jsonl"], 2 << 20);
                else if ([route isEqualToString:@"/assets"]) { body = json_data(dm_tvos_asset_status()); type = @"application/json"; }
                else if ([route isEqualToString:@"/cloud-save"]) { body = json_data(dm_tvos_cloud_save_status()); type = @"application/json"; }
                else if ([route isEqualToString:@"/log"]) { body = file_tail([directory stringByAppendingPathComponent:@"log.txt"], 1 << 20); type = @"text/plain"; }
                else if ([route isEqualToString:@"/memory"]) body = file_tail([directory stringByAppendingPathComponent:@"memory.tsv"], 256 << 10);
                else if ([route isEqualToString:@"/receipts"]) body = file_tail([directory stringByAppendingPathComponent:@"input.jsonl"], 1 << 20);
                else if ([route isEqualToString:@"/frame.png"]) { body = file_tail([directory stringByAppendingPathComponent:@"latest.png"], 16 << 20); type = @"image/png"; }
                else if ([route isEqualToString:@"/save-index"]) body = json_data([NSFileManager.defaultManager subpathsAtPath:[directory stringByAppendingPathComponent:@"save"]] ?: @[]);
                else if ([route hasPrefix:@"/save/"]) body = save_file(directory, [[route substringFromIndex:6] stringByRemovingPercentEncoding]);
                if (body) code = 200;
            }
        }
        if (!body) body = json_data(@{@"status": @(code)});
        NSString *header = [NSString stringWithFormat:@"HTTP/1.1 %d Result\r\nContent-Type: %@\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n", code, type, (unsigned long)body.length];
        if (send_all(connection, [header dataUsingEncoding:NSUTF8StringEncoding])) send_all(connection, body);
    });
}
