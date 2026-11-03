#import "performance_hud.h"
#import <QuartzCore/QuartzCore.h>
#include <mach/mach.h>
#include <sys/resource.h>
#include "display.h"
#include "performance_metrics.h"

uint64_t mtl_present_count(void);

@interface DmPerformanceHUD : UIView {
    dm_performance_counter _counter;
    dm_performance_rates _rates;
    dm_performance_history _history;
    uint64_t _footprint;
    double _sampleTime;
    BOOL _memoryValid;
    unsigned _options, _cores;
}
@property(nonatomic, strong) NSTimer *timer;
- (void)sample;
@end

void dm_tvos_performance_layout(UIView *view) {
    UIView *parent = view.superview;
    if (!parent) return;
    CGRect safe = UIEdgeInsetsInsetRect(parent.bounds, parent.safeAreaInsets);
    CGFloat width = MIN(346, MAX(0, safe.size.width - 32)), height = 162;
    unsigned position = (dm_tvos_display_options() & DM_TV_DISPLAY_POSITION_MASK) >> DM_TV_DISPLAY_POSITION_SHIFT;
    BOOL right = position == DM_TV_TOP_RIGHT || position == DM_TV_BOTTOM_RIGHT;
    BOOL bottom = position == DM_TV_BOTTOM_LEFT || position == DM_TV_BOTTOM_RIGHT;
    view.frame = CGRectMake(right ? CGRectGetMaxX(safe) - width - 16 : CGRectGetMinX(safe) + 16,
                           bottom ? CGRectGetMaxY(safe) - height - 16 : CGRectGetMinY(safe) + 16, width, height);
}

@implementation DmPerformanceHUD
- (instancetype)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.backgroundColor = [UIColor colorWithRed:12.0 / 255 green:14.0 / 255 blue:19.0 / 255 alpha:0.76];
        self.opaque = NO; self.userInteractionEnabled = NO;
        self.layer.cornerRadius = 12; self.layer.borderWidth = 1;
        self.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.14].CGColor;
        self.layer.masksToBounds = YES;
        self.isAccessibilityElement = YES;
        _rates = (dm_performance_rates){NAN, NAN};
        _cores = (unsigned)MAX(1, NSProcessInfo.processInfo.activeProcessorCount);
        _options = dm_tvos_display_options();
        self.hidden = !(_options & DM_TV_DISPLAY_FPS);
    }
    return self;
}
- (void)didMoveToWindow {
    [super didMoveToWindow];
    [self.timer invalidate]; self.timer = nil;
    if (!self.window) return;
    _counter.valid = 0;
    [self sample];
    __weak DmPerformanceHUD *weak = self;
    self.timer = [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer *timer) {
        DmPerformanceHUD *view = weak;
        if (view) [view sample]; else [timer invalidate];
    }];
    self.timer.tolerance = 0.1;
}
- (void)dealloc { [_timer invalidate]; }
- (void)sample {
    unsigned options = dm_tvos_display_options();
    if (options != _options) {
        _options = options; dm_tvos_performance_layout(self);
        [self.superview setNeedsLayout];
    }
    self.hidden = !(options & DM_TV_DISPLAY_FPS);
    if (self.hidden || UIApplication.sharedApplication.applicationState != UIApplicationStateActive) {
        _counter.valid = 0; return;
    }
    struct rusage usage = {0};
    double cpu = NAN;
    if (!getrusage(RUSAGE_SELF, &usage))
        cpu = usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 + usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
    _sampleTime = CACurrentMediaTime();
    _rates = dm_performance_update(&_counter, _sampleTime, mtl_present_count(), cpu, _cores);
    task_vm_info_data_t vm = {0}; mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    _memoryValid = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &count) == KERN_SUCCESS;
    if (_memoryValid) { _footprint = vm.phys_footprint; dm_performance_push(&_history, _sampleTime, _footprint); }
    [self setNeedsDisplay];
}
- (void)drawMetric:(NSString *)title value:(NSString *)value unit:(NSString *)unit x:(CGFloat)x {
    UIColor *muted = [UIColor colorWithWhite:0.70 alpha:1];
    [title drawAtPoint:CGPointMake(x, 16) withAttributes:@{NSFontAttributeName: [UIFont systemFontOfSize:11 weight:UIFontWeightSemibold], NSForegroundColorAttributeName: muted, NSKernAttributeName: @1.1}];
    NSMutableAttributedString *text = [[NSMutableAttributedString alloc] initWithString:value attributes:@{
        NSFontAttributeName: [UIFont monospacedDigitSystemFontOfSize:31 weight:UIFontWeightSemibold], NSForegroundColorAttributeName: UIColor.whiteColor}];
    [text appendAttributedString:[[NSAttributedString alloc] initWithString:[@" " stringByAppendingString:unit] attributes:@{
        NSFontAttributeName: [UIFont systemFontOfSize:13 weight:UIFontWeightMedium], NSForegroundColorAttributeName: muted}]];
    [text drawAtPoint:CGPointMake(x, 32)];
}
- (void)drawRect:(CGRect)rect {
    (void)rect;
    NSString *fps = isfinite(_rates.fps) ? [NSString stringWithFormat:@"%.0f", _rates.fps] : @"--";
    NSString *cpu = isfinite(_rates.cpu_percent) ? [NSString stringWithFormat:@"%.0f", _rates.cpu_percent] : @"--";
    NSString *memory = _memoryValid ? [NSString stringWithFormat:@"%.2f", _footprint / 1073741824.0] : @"--";
    [self drawMetric:@"FPS" value:fps unit:@"fps" x:18];
    [self drawMetric:@"CPU" value:cpu unit:@"%" x:106];
    [self drawMetric:@"MEMORY" value:memory unit:@"GiB" x:200];
    self.accessibilityLabel = [NSString stringWithFormat:@"%@ frames per second, CPU %@ percent, memory %@ gibibytes", fps, cpu, memory];

    double oldest = _sampleTime - 60, low = INFINITY, high = 0;
    for (unsigned i = 0; i < _history.count; i++) {
        dm_performance_point point = dm_performance_at(&_history, i);
        if (point.time < oldest) continue;
        double gib = point.bytes / 1073741824.0;
        low = fmin(low, gib); high = fmax(high, gib);
    }
    if (!isfinite(low)) return;
    low = floor(low * 4) / 4; high = ceil(high * 4) / 4;
    if (high - low < 0.5) { low = fmax(0, high - 0.5); high = fmax(high, low + 0.5); }
    NSDictionary *small = @{NSFontAttributeName: [UIFont systemFontOfSize:10 weight:UIFontWeightMedium],
                            NSForegroundColorAttributeName: [UIColor colorWithWhite:0.70 alpha:1]};
    [@"MEMORY · 60s" drawAtPoint:CGPointMake(18, 88) withAttributes:small];
    NSString *range = [NSString stringWithFormat:@"%.2f–%.2f GiB", low, high];
    [range drawAtPoint:CGPointMake(self.bounds.size.width - 18 - [range sizeWithAttributes:small].width, 88) withAttributes:small];
    CGRect chart = CGRectMake(18, 106, self.bounds.size.width - 36, 44);
    UIBezierPath *grid = [UIBezierPath bezierPath];
    for (unsigned i = 0; i < 3; i++) {
        CGFloat y = chart.origin.y + 3 + i * (chart.size.height - 6) / 2;
        [grid moveToPoint:CGPointMake(chart.origin.x, y)]; [grid addLineToPoint:CGPointMake(CGRectGetMaxX(chart), y)];
    }
    [[UIColor colorWithWhite:1 alpha:0.08] setStroke]; grid.lineWidth = 1; [grid stroke];
    UIBezierPath *line = [UIBezierPath bezierPath];
    double previous = -INFINITY; CGPoint last = CGPointZero; BOOL hasPoint = NO;
    for (unsigned i = 0; i < _history.count; i++) {
        dm_performance_point point = dm_performance_at(&_history, i);
        if (point.time < oldest) continue;
        double value = (point.bytes / 1073741824.0 - low) / (high - low);
        last = CGPointMake(chart.origin.x + (point.time - oldest) / 60 * chart.size.width,
                           CGRectGetMaxY(chart) - value * chart.size.height);
        if (point.time - previous > 2.5) [line moveToPoint:last]; else [line addLineToPoint:last];
        previous = point.time; hasPoint = YES;
    }
    UIColor *accent = [UIColor colorNamed:@"LoadingAccent"] ?: [UIColor colorWithRed:1 green:0.26 blue:0.34 alpha:1];
    [accent setStroke]; line.lineWidth = 2.2; line.lineJoinStyle = kCGLineJoinRound; line.lineCapStyle = kCGLineCapRound; [line stroke];
    if (hasPoint) { [accent setFill]; [[UIBezierPath bezierPathWithOvalInRect:CGRectMake(last.x - 2.5, last.y - 2.5, 5, 5)] fill]; }
}
@end

UIView *dm_tvos_performance_view(void) { return [[DmPerformanceHUD alloc] initWithFrame:CGRectMake(0, 0, 346, 162)]; }
