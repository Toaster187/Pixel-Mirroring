#ifdef __APPLE__
#include "cocoa_window.h"
#import <Cocoa/Cocoa.h>

@interface PixelMirroringWindowDelegate : NSObject <NSWindowDelegate>
@property (assign) BOOL isLandscape;
@property (assign) CGFloat aspectRatio;
@end

@implementation PixelMirroringWindowDelegate

- (NSRect)windowWillUseStandardFrame:(NSWindow *)window defaultFrame:(NSRect)newFrame {
    // Custom zoom logic: If portrait, only maximize height, keep aspect ratio width
    if (!self.isLandscape && self.aspectRatio > 0) {
        NSScreen *screen = window.screen ?: [NSScreen mainScreen];
        NSRect visibleFrame = screen.visibleFrame;
        
        CGFloat targetHeight = visibleFrame.size.height;
        CGFloat targetWidth = targetHeight * self.aspectRatio;
        
        // Center horizontally
        CGFloat x = visibleFrame.origin.x + (visibleFrame.size.width - targetWidth) / 2.0;
        
        return NSMakeRect(x, visibleFrame.origin.y, targetWidth, targetHeight);
    }
    // Landscape: normal full screen zoom
    return newFrame;
}

@end

@interface PixelMirroringView : NSView
@end

@implementation PixelMirroringView
- (void)drawRect:(NSRect)dirtyRect {
    [super drawRect:dirtyRect];
    
    // Draw the rounded phone background (dark gray)
    [[NSColor colorWithCalibratedRed:30.0/255.0 green:30.0/255.0 blue:30.0/255.0 alpha:1.0] setFill];
    NSRect bounds = self.bounds;
    
    // In macOS, the window itself has rounded corners. 
    // We just fill the content area, which gets clipped to the window's rounded corners automatically.
    NSRectFill(bounds);
    
    // Placeholder text
    NSString *text = @"📱 Phone Screen Video Area";
    NSDictionary *attributes = @{
        NSForegroundColorAttributeName: [NSColor colorWithCalibratedWhite:0.6 alpha:1.0],
        NSFontAttributeName: [NSFont systemFontOfSize:14]
    };
    NSSize textSize = [text sizeWithAttributes:attributes];
    NSPoint textPoint = NSMakePoint(NSMidX(bounds) - textSize.width / 2.0, NSMidY(bounds) - textSize.height / 2.0);
    [text drawAtPoint:textPoint withAttributes:attributes];
}
@end


namespace pm::window {

// Factory implementation
#ifndef _WIN32 // Ensure we only define this once if compiling on macOS
std::unique_ptr<IWindow> create_window(int width, int height, const std::string& title) {
    return std::make_unique<CocoaWindow>(width, height, title);
}
#endif

CocoaWindow::CocoaWindow(int width, int height, const std::string& title)
    : width_(width), height_(height), title_(title) {
}

CocoaWindow::~CocoaWindow() {
    // Only closed, not released: the NSWindow is held through a __bridge void*, so
    // its lifetime is left to whichever memory model the file is compiled under.
    // (macOS is not a buildable target today — see CLAUDE.md.)
    if (window_) {
        NSWindow* win = (__bridge NSWindow*)window_;
        [win close];
    }
}

bool CocoaWindow::create() {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    NSRect frame = NSMakeRect(0, 0, width_, height_);
    NSUInteger styleMask = NSWindowStyleMaskTitled | 
                           NSWindowStyleMaskClosable | 
                           NSWindowStyleMaskMiniaturizable | 
                           NSWindowStyleMaskResizable;

    NSWindow* win = [[NSWindow alloc] initWithContentRect:frame
                                                styleMask:styleMask
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
    
    [win setTitle:[NSString stringWithUTF8String:title_.c_str()]];
    [win center];

    // Delegate for custom zoom behavior
    PixelMirroringWindowDelegate *delegate = [[PixelMirroringWindowDelegate alloc] init];
    [win setDelegate:delegate];
    
    // Set custom view
    PixelMirroringView* view = [[PixelMirroringView alloc] initWithFrame:frame];
    [win setContentView:view];

    // Store pointers safely
    window_ = (__bridge_retained void*)win;
    view_ = (__bridge_retained void*)view;

    return true;
}

void CocoaWindow::show() {
    if (!window_) return;
    NSWindow* win = (__bridge NSWindow*)window_;
    [win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

void CocoaWindow::process_messages() {
    [NSApp run];
}

void CocoaWindow::set_aspect_ratio(double ratio) {
    if (!window_) return;
    NSWindow* win = (__bridge NSWindow*)window_;
    
    NSSize size = NSMakeSize(100, 100 / ratio);
    [win setContentAspectRatio:size];
    
    PixelMirroringWindowDelegate *delegate = (PixelMirroringWindowDelegate *)[win delegate];
    delegate.aspectRatio = ratio;
}

void CocoaWindow::set_orientation(bool landscape) {
    is_landscape_ = landscape;
    if (!window_) return;
    NSWindow* win = (__bridge NSWindow*)window_;
    PixelMirroringWindowDelegate *delegate = (PixelMirroringWindowDelegate *)[win delegate];
    delegate.isLandscape = landscape;
}

} // namespace pm::window
#endif
