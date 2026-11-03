#import "loading.h"
#import "ui_text.h"
#import <QuartzCore/QuartzCore.h>

@interface DmTVLoadingView : UIView
@property (nonatomic, strong) UIImageView *cover;
@property (nonatomic, strong) CAGradientLayer *shade;
@property (nonatomic, strong) UIView *track;
@property (nonatomic, strong) UIView *fill;
@property (nonatomic, strong) UILabel *status;
@property (nonatomic, strong) UILabel *percent;
@property (nonatomic) double fraction;
@end

@implementation DmTVLoadingView
- (instancetype)initWithFrame:(CGRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.backgroundColor = [UIColor colorNamed:@"LoadingBackground"] ?:
        [UIColor colorWithRed:0.04 green:0.13 blue:0.13 alpha:1];
    self.userInteractionEnabled = NO;
    self.cover = [[UIImageView alloc] initWithImage:[UIImage imageNamed:@"GameCover"]];
    self.cover.contentMode = UIViewContentModeScaleAspectFill;
    self.cover.clipsToBounds = YES;
    [self addSubview:self.cover];
    self.shade = [CAGradientLayer layer];
    UIColor *shade = [UIColor colorNamed:@"LoadingShade"] ?: [UIColor colorWithRed:0.02 green:0.07 blue:0.07 alpha:1];
    self.shade.colors = @[(id)UIColor.clearColor.CGColor, (id)[UIColor colorWithWhite:0 alpha:0.05].CGColor,
                         (id)[shade colorWithAlphaComponent:0.95].CGColor];
    self.shade.locations = @[@0, @0.62, @1];
    [self.layer addSublayer:self.shade];
    self.track = [UIView new]; self.track.backgroundColor = [UIColor colorWithWhite:1 alpha:0.18];
    self.track.layer.cornerRadius = 3;
    [self addSubview:self.track];
    self.fill = [UIView new];
    self.fill.backgroundColor = [UIColor colorNamed:@"LoadingAccent"] ?: [UIColor colorWithRed:0.55 green:1 blue:0.84 alpha:1];
    self.fill.layer.cornerRadius = 3;
    UIColor *shadow = [UIColor colorNamed:@"LoadingShadow"] ?: [UIColor colorWithRed:0.2 green:1 blue:0.7 alpha:1];
    self.fill.layer.shadowColor = shadow.CGColor;
    self.fill.layer.shadowOpacity = 0.95; self.fill.layer.shadowRadius = 10;
    self.fill.layer.shadowOffset = CGSizeZero;
    [self addSubview:self.fill];
    self.status = [UILabel new]; self.percent = [UILabel new];
    for (UILabel *label in @[self.status, self.percent]) {
        label.textColor = UIColor.whiteColor;
        label.font = [UIFont systemFontOfSize:26 weight:UIFontWeightMedium];
        [self addSubview:label];
    }
    self.status.text = dm_tvos_text(@"Checking local game files...");
    self.status.numberOfLines = 2;
    self.percent.font = [UIFont monospacedDigitSystemFontOfSize:28 weight:UIFontWeightMedium];
    self.percent.textAlignment = NSTextAlignmentRight;
    return self;
}
- (void)layoutSubviews {
    [super layoutSubviews];
    self.cover.frame = self.bounds;
    [CATransaction begin]; [CATransaction setDisableActions:YES]; self.shade.frame = self.bounds; [CATransaction commit];
    CGRect safe = UIEdgeInsetsInsetRect(self.bounds, self.safeAreaInsets);
    CGFloat width = MIN(safe.size.width * 0.82, 1500), x = CGRectGetMidX(safe) - width / 2;
    CGFloat y = MIN(CGRectGetMaxY(safe) - 76, self.bounds.size.height * 0.90);
    self.track.frame = CGRectMake(x, y, width, 6);
    self.fill.frame = CGRectMake(x, y, width * self.fraction, 6);
    self.fill.hidden = self.fraction <= 0;
    self.status.frame = CGRectMake(x, y - 64, width - 130, 58);
    self.percent.frame = CGRectMake(x + width - 120, y - 64, 120, 58);
}
@end

UIView *dm_tvos_loading_view(CGRect frame) { return [[DmTVLoadingView alloc] initWithFrame:frame]; }

void dm_tvos_loading_update(UIView *view, NSDictionary *status) {
    DmTVLoadingView *loading = (DmTVLoadingView *)view;
    loading.status.text = dm_tvos_text(status[@"message"] ?: @"Preparing game resources...");
    NSNumber *fraction = status[@"fraction"];
    if (fraction) loading.fraction = MAX(0, MIN(1, fraction.doubleValue));
    BOOL failed = [status[@"stage"] isEqualToString:@"failed"];
    loading.percent.text = fraction && !failed ? [NSString stringWithFormat:@"%.0f%%", loading.fraction * 100] : @"";
    loading.accessibilityLabel = loading.status.text;
    loading.accessibilityValue = loading.percent.text;
    [loading setNeedsLayout];
}
