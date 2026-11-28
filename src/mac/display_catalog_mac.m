/* =====================================================================
 *  HopDesk - macOS Display Inventory, m1ddc Probe & Identify HUD
 * ===================================================================== */
#import <Cocoa/Cocoa.h>
#import <ApplicationServices/ApplicationServices.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "mac_app.h"
#include "../core/display_catalog.h"

static pthread_mutex_t s_inv_lock = PTHREAD_MUTEX_INITIALIZER;
static hop_display_inventory s_inv;
static int s_inv_loaded;
static int s_probe_busy;
static NSMutableArray<NSPanel *> *s_identify_panels;

static NSString *find_m1ddc_path(void) {
    NSArray<NSString *> *candidates = @[
        @"/opt/homebrew/bin/m1ddc",
        @"/usr/local/bin/m1ddc",
        @"/usr/bin/m1ddc"
    ];
    NSFileManager *fm = [NSFileManager defaultManager];
    for (NSString *path in candidates) {
        if ([fm isExecutableFileAtPath:path]) return path;
    }
    return nil;
}

static NSString *run_tool_capture(NSString *execPath, NSArray<NSString *> *args, double timeoutSec) {
    if (!execPath.length) return nil;
    NSTask *task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:execPath];
    task.arguments = args ?: @[];
    NSPipe *outPipe = [NSPipe pipe];
    task.standardOutput = outPipe;
    task.standardError = [NSPipe pipe];
    NSError *err = nil;
    if (![task launchAndReturnError:&err]) return nil;

    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:timeoutSec];
    while (task.running && [deadline timeIntervalSinceNow] > 0) {
        [NSThread sleepForTimeInterval:0.05];
    }
    if (task.running) {
        [task terminate];
        return nil;
    }
    if (task.terminationStatus != 0) return nil;
    NSData *data = [[outPipe fileHandleForReading] readDataToEndOfFile];
    if (!data.length) return nil;
    return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
}

static void populate_cg_fallback(hop_display_inventory *dst) {
    memset(dst, 0, sizeof(*dst));
    dst->ddc_tool_ready = find_m1ddc_path() != nil ? 1 : 0;
    CGDirectDisplayID ids[HOP_DISPLAY_MAX_DEVICES];
    uint32_t count = 0;
    if (CGGetActiveDisplayList(HOP_DISPLAY_MAX_DEVICES, ids, &count) != kCGErrorSuccess)
        return;
    for (uint32_t i = 0; i < count && dst->count < HOP_DISPLAY_MAX_DEVICES; ++i) {
        hop_display_info *it = &dst->items[dst->count];
        CGRect bounds = CGDisplayBounds(ids[i]);
        it->index = (int32_t)(dst->count + 1);
        it->is_primary = CGDisplayIsMain(ids[i]) ? 1 : 0;
        it->attached = 1;
        it->is_builtin = CGDisplayIsBuiltin(ids[i]) ? 1 : 0;
        it->width = (int32_t)bounds.size.width;
        it->height = (int32_t)bounds.size.height;
        it->pos_x = (int32_t)bounds.origin.x;
        it->pos_y = (int32_t)bounds.origin.y;
        it->current_input = -1;
        if (!it->is_builtin) {
            int ext_idx = 0;
            for (int k = 0; k < dst->count; ++k) {
                if (!dst->items[k].is_builtin) ++ext_idx;
            }
            snprintf(it->device_id, sizeof(it->device_id), "%d", ext_idx + 1);
        }
        snprintf(it->friendly_name, sizeof(it->friendly_name), "%s",
                 it->is_builtin ? "Mac 内置显示器" : "外接显示器");
        dst->count++;
    }
}

static void collect_nsscreens_on_main(hop_display_inventory *dst) {
    memset(dst, 0, sizeof(*dst));
    dst->ddc_tool_ready = find_m1ddc_path() != nil ? 1 : 0;
    NSArray<NSScreen *> *screens = [NSScreen screens];
    CGDirectDisplayID mainID = CGMainDisplayID();
    for (NSScreen *screen in screens) {
        if (dst->count >= HOP_DISPLAY_MAX_DEVICES) break;
        NSNumber *num = screen.deviceDescription[@"NSScreenNumber"];
        CGDirectDisplayID did = num ? (CGDirectDisplayID)num.unsignedIntValue : 0;
        CGRect bounds = did ? CGDisplayBounds(did) : NSRectToCGRect(screen.frame);
        hop_display_info *it = &dst->items[dst->count];
        it->index = (int32_t)(dst->count + 1);
        it->is_primary = (did == mainID) ? 1 : 0;
        it->attached = 1;
        it->is_builtin = (did && CGDisplayIsBuiltin(did)) ? 1 : 0;
        it->width = (int32_t)bounds.size.width;
        it->height = (int32_t)bounds.size.height;
        it->pos_x = (int32_t)bounds.origin.x;
        it->pos_y = (int32_t)bounds.origin.y;
        it->current_input = -1;
        if (!it->is_builtin) {
            int ext_idx = 0;
            for (int k = 0; k < dst->count; ++k) {
                if (!dst->items[k].is_builtin) ++ext_idx;
            }
            snprintf(it->device_id, sizeof(it->device_id), "%d", ext_idx + 1);
        }
        NSString *name = nil;
        if (@available(macOS 10.15, *)) name = screen.localizedName;
        if (name.length) {
            snprintf(it->friendly_name, sizeof(it->friendly_name), "%s", name.UTF8String);
        } else {
            snprintf(it->friendly_name, sizeof(it->friendly_name), "%s",
                     it->is_builtin ? "Mac 内置显示器" : "外接显示器");
        }
        dst->count++;
    }
}

static void probe_m1ddc_into_inventory(hop_display_inventory *inv) {
    NSString *m1ddc = find_m1ddc_path();
    inv->ddc_tool_ready = m1ddc != nil ? 1 : 0;
    if (!m1ddc) {
        snprintf(inv->probe_note, sizeof(inv->probe_note), "%s",
                 "未检测到 m1ddc，请先运行 brew install m1ddc 以启用 Mac 侧 DDC 切源与识别。");
        return;
    }

    NSString *listOut = run_tool_capture(m1ddc, @[@"display", @"list"], 3.0);
    int ddc_entries = 0;
    int read_ok = 0;
    if (listOut.length) {
        /* Format: "[1] BenQ PD2700U (UUID...)" */
        NSArray<NSString *> *lines = [listOut componentsSeparatedByCharactersInSet:
            [NSCharacterSet newlineCharacterSet]];
        int m1_slot = 0;
        for (NSString *raw in lines) {
            NSString *line = [raw stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
            if (![line hasPrefix:@"["]) continue;
            NSRange close = [line rangeOfString:@"]"];
            if (close.location == NSNotFound || close.location < 2) continue;
            int m1_idx = [[line substringWithRange:NSMakeRange(1, close.location - 1)] intValue];
            if (m1_idx < 1 || m1_idx > 16) continue;
            NSString *rest = [[line substringFromIndex:close.location + 1]
                stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
            NSRange paren = [rest rangeOfString:@" (" options:NSBackwardsSearch];
            NSString *model = (paren.location != NSNotFound) ? [rest substringToIndex:paren.location] : rest;
            model = [model stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];

            int matched = -1;
            for (int i = 0; i < inv->count; ++i) {
                if (inv->items[i].is_builtin) continue;
                if (model.length && strcasecmp(inv->items[i].friendly_name, model.UTF8String) == 0) {
                    matched = i;
                    break;
                }
            }
            if (matched < 0) {
                int ext_seen = 0;
                for (int i = 0; i < inv->count; ++i) {
                    if (inv->items[i].is_builtin) continue;
                    if (ext_seen == m1_slot) { matched = i; break; }
                    ++ext_seen;
                }
            }
            if (matched < 0 && inv->count < HOP_DISPLAY_MAX_DEVICES) {
                matched = inv->count++;
                memset(&inv->items[matched], 0, sizeof(inv->items[matched]));
                inv->items[matched].index = matched + 1;
                inv->items[matched].attached = 1;
                inv->items[matched].current_input = -1;
            }
            if (matched >= 0) {
                snprintf(inv->items[matched].device_id, sizeof(inv->items[matched].device_id), "%d", m1_idx);
                if (model.length) {
                    snprintf(inv->items[matched].friendly_name, sizeof(inv->items[matched].friendly_name),
                             "%s", model.UTF8String);
                }
                NSString *inOut = run_tool_capture(m1ddc,
                    @[@"display", [NSString stringWithFormat:@"%d", m1_idx], @"get", @"input"], 3.0);
                if (inOut.length) {
                    NSString *trimmed = [inOut stringByTrimmingCharactersInSet:
                        [NSCharacterSet whitespaceAndNewlineCharacterSet]];
                    char *end = NULL;
                    long val = strtol(trimmed.UTF8String, &end, 0);
                    if (end && end != trimmed.UTF8String && *end == '\0' && val >= 0 && val <= 65535) {
                        inv->items[matched].current_input = (int32_t)val;
                        ++read_ok;
                    }
                }
                ++ddc_entries;
            }
            ++m1_slot;
        }
    }

    if (read_ok > 0) {
        snprintf(inv->probe_note, sizeof(inv->probe_note),
                 "m1ddc 已就绪：识别 %d 台显示器，成功读取 %d 台外接屏当前输入源。",
                 inv->count, read_ok);
    } else if (ddc_entries > 0 || inv->count > 0) {
        snprintf(inv->probe_note, sizeof(inv->probe_note),
                 "m1ddc 已就绪：识别 %d 台显示器，暂未读回当前输入源（可能当前未切到 Mac 信号）。",
                 inv->count);
    } else {
        snprintf(inv->probe_note, sizeof(inv->probe_note), "%s",
                 "m1ddc 已就绪，但未检测到在线外接显示器。");
    }
}

void mac_display_get_inventory(hop_display_inventory *out) {
    if (!out) return;
    int need_async = 0;
    pthread_mutex_lock(&s_inv_lock);
    if (!s_inv_loaded) {
        populate_cg_fallback(&s_inv);
        s_inv_loaded = 1;
        need_async = !s_probe_busy;
    }
    *out = s_inv;
    pthread_mutex_unlock(&s_inv_lock);
    if (need_async) (void)mac_display_probe_async();
}

int mac_display_probe_async(void) {
    pthread_mutex_lock(&s_inv_lock);
    if (s_probe_busy) {
        pthread_mutex_unlock(&s_inv_lock);
        return 1;
    }
    s_probe_busy = 1;
    s_inv.probing = 1;
    snprintf(s_inv.probe_note, sizeof(s_inv.probe_note), "%s", "正在识别显示器与 m1ddc 输入源…");
    pthread_mutex_unlock(&s_inv_lock);

    dispatch_async(dispatch_get_main_queue(), ^{
        hop_display_inventory base;
        collect_nsscreens_on_main(&base);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            hop_display_inventory fresh = base;
            probe_m1ddc_into_inventory(&fresh);
            fresh.probing = 0;
            pthread_mutex_lock(&s_inv_lock);
            s_inv = fresh;
            s_inv_loaded = 1;
            s_probe_busy = 0;
            pthread_mutex_unlock(&s_inv_lock);
        });
    });
    return 1;
}

int mac_display_identify(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (s_identify_panels) {
            for (NSPanel *p in s_identify_panels) [p orderOut:nil];
            [s_identify_panels removeAllObjects];
        } else {
            s_identify_panels = [NSMutableArray array];
        }

        NSArray<NSScreen *> *screens = [NSScreen screens];
        CGDirectDisplayID mainID = CGMainDisplayID();
        NSUInteger idx = 0;
        for (NSScreen *screen in screens) {
            ++idx;
            NSNumber *num = screen.deviceDescription[@"NSScreenNumber"];
            CGDirectDisplayID did = num ? (CGDirectDisplayID)num.unsignedIntValue : 0;
            BOOL builtin = did && CGDisplayIsBuiltin(did);
            BOOL primary = did == mainID;
            NSString *name = nil;
            if (@available(macOS 10.15, *)) name = screen.localizedName;
            if (!name.length) name = builtin ? @"内置显示器" : @"外接显示器";

            NSRect sf = screen.frame;
            CGFloat w = 360.0, h = 176.0;
            NSRect frame = NSMakeRect(
                sf.origin.x + (sf.size.width - w) * 0.5,
                sf.origin.y + (sf.size.height - h) * 0.5,
                w, h);
            NSPanel *panel = [[NSPanel alloc]
                initWithContentRect:frame
                          styleMask:(NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel)
                            backing:NSBackingStoreBuffered
                              defer:NO];
            panel.opaque = NO;
            panel.backgroundColor = [NSColor clearColor];
            panel.hasShadow = YES;
            panel.ignoresMouseEvents = YES;
            panel.level = NSStatusWindowLevel;
            panel.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                       NSWindowCollectionBehaviorTransient |
                                       NSWindowCollectionBehaviorIgnoresCycle;

            NSView *root = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, w, h)];
            root.wantsLayer = YES;
            root.layer.backgroundColor = [NSColor colorWithSRGBRed:0.09 green:0.11 blue:0.15 alpha:0.92].CGColor;
            root.layer.cornerRadius = 18.0;
            root.layer.borderWidth = 2.5;
            root.layer.borderColor = [NSColor colorWithSRGBRed:0.22 green:0.55 blue:0.99 alpha:0.95].CGColor;

            NSTextField *numLabel = [NSTextField labelWithString:[NSString stringWithFormat:@"%lu", (unsigned long)idx]];
            numLabel.frame = NSMakeRect(16, 64, w - 32, 92);
            numLabel.alignment = NSTextAlignmentCenter;
            numLabel.font = [NSFont systemFontOfSize:68 weight:NSFontWeightBold];
            numLabel.textColor = [NSColor whiteColor];

            NSString *roleText = builtin ? @"内置屏" : (primary ? @"主屏 · 外接" : @"扩展屏");
            NSTextField *subLabel = [NSTextField labelWithString:
                [NSString stringWithFormat:@"%@ · %@", name, roleText]];
            subLabel.frame = NSMakeRect(16, 20, w - 32, 34);
            subLabel.alignment = NSTextAlignmentCenter;
            subLabel.font = [NSFont systemFontOfSize:17 weight:NSFontWeightMedium];
            subLabel.textColor = [NSColor colorWithSRGBRed:0.78 green:0.84 blue:0.93 alpha:1.0];
            subLabel.lineBreakMode = NSLineBreakByTruncatingTail;

            [root addSubview:numLabel];
            [root addSubview:subLabel];
            panel.contentView = root;
            [panel orderFrontRegardless];
            [s_identify_panels addObject:panel];
        }

        NSArray<NSPanel *> *captured = [s_identify_panels copy];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(3.0 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            for (NSPanel *p in captured) {
                [p orderOut:nil];
                [s_identify_panels removeObject:p];
            }
        });
    });
    return 1;
}
