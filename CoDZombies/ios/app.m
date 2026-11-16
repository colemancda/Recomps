/* The iOS app around the recompiled game.
 *
 * The game's start routine never returns (its main loop is inside), so it runs
 * on a thread of its own with a large stack (the lifted code recurses as the
 * original did, and secondary threads get 512 KB by default). The UI thread
 * keeps UIKit: it turns touches into touch events for the game (multi-touch).
 *
 * Drawing: the game draws a 480x320 picture (SCREEN_W x SCREEN_H) with
 * OpenGL ES 1.1. The view is the largest 3:2 rectangle that fits the window,
 * centred, and its content scale makes its EAGL drawable exactly 480x320; Core
 * Animation scales that up to the view. The context is made on the game thread
 * when the game calls eglMakeCurrent, and eglSwapBuffers presents.
 *
 * The bundle holds the original executable as "kiwi" (the loader reads its
 * segments) and the game's .dz archives; saves go to Documents.
 */
#define GLES_SILENCE_DEPRECATION
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>
#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES1/gl.h>
#import <OpenGLES/ES1/glext.h>
#include <pthread.h>

#include "rt.h"

@interface GameView : UIView
@end

static GameView *game_view;
static EAGLContext *context;
static GLuint fbo, color_rb, depth_rb;

/* ------------------------------------------------------- GL (game thread) */
void ios_gl_make_current(void) {
    if (context) return;
    context = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES1];
    if (!context || ![EAGLContext setCurrentContext:context]) {
        rt_log("iOS: no OpenGL ES 1.1 context");
        return;
    }
    __block CAEAGLLayer *layer;
    dispatch_sync(dispatch_get_main_queue(), ^{ layer = (CAEAGLLayer *)game_view.layer; });
    glGenFramebuffersOES(1, &fbo);
    glBindFramebufferOES(GL_FRAMEBUFFER_OES, fbo);
    glGenRenderbuffersOES(1, &color_rb);
    glBindRenderbufferOES(GL_RENDERBUFFER_OES, color_rb);
    [context renderbufferStorage:GL_RENDERBUFFER_OES fromDrawable:layer];
    glFramebufferRenderbufferOES(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_RENDERBUFFER_OES, color_rb);
    GLint w = 0, h = 0;
    glGetRenderbufferParameterivOES(GL_RENDERBUFFER_OES, GL_RENDERBUFFER_WIDTH_OES, &w);
    glGetRenderbufferParameterivOES(GL_RENDERBUFFER_OES, GL_RENDERBUFFER_HEIGHT_OES, &h);
    glGenRenderbuffersOES(1, &depth_rb);
    glBindRenderbufferOES(GL_RENDERBUFFER_OES, depth_rb);
    glRenderbufferStorageOES(GL_RENDERBUFFER_OES, GL_DEPTH24_STENCIL8_OES, w, h);
    glFramebufferRenderbufferOES(GL_FRAMEBUFFER_OES, GL_DEPTH_ATTACHMENT_OES, GL_RENDERBUFFER_OES, depth_rb);
    glFramebufferRenderbufferOES(GL_FRAMEBUFFER_OES, GL_STENCIL_ATTACHMENT_OES, GL_RENDERBUFFER_OES, depth_rb);
    glBindRenderbufferOES(GL_RENDERBUFFER_OES, color_rb);
    rt_log("iOS: drawable %dx%d, framebuffer status %#x", w, h, glCheckFramebufferStatusOES(GL_FRAMEBUFFER_OES));
    glViewport(0, 0, w, h);
}

void ios_gl_present(void) {
    if (!context) return;
    glBindRenderbufferOES(GL_RENDERBUFFER_OES, color_rb);
    [context presentRenderbuffer:GL_RENDERBUFFER_OES];
}

/* ------------------------------------------------------------- the game */
static void *game_thread(void *arg) {
    (void)arg;
    @autoreleasepool {
        NSString *app = [[NSBundle mainBundle] resourcePath];
        NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
        rt_log("iOS: game data %s, saves %s", app.fileSystemRepresentation, docs.fileSystemRepresentation);
        /* imports not written yet return 0 (and are logged) rather than stopping the game */
        setenv("RT_MISSING", "zero", 0);
        int r = codz_run(app.fileSystemRepresentation, docs.fileSystemRepresentation);
        rt_log("iOS: the game returned %d", r);
    }
    return NULL;
}

static void start_game(void) {
    static int started;
    if (started++) return;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);
    pthread_t t;
    pthread_create(&t, &attr, game_thread, NULL);
    pthread_attr_destroy(&attr);
}

/* ------------------------------------------------------------ the view */
@implementation GameView
+ (Class)layerClass {
    return [CAEAGLLayer class];
}
- (instancetype)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        CAEAGLLayer *layer = (CAEAGLLayer *)self.layer;
        layer.opaque = YES;
        layer.drawableProperties = @{kEAGLDrawablePropertyRetainedBacking: @NO,
                                     kEAGLDrawablePropertyColorFormat: kEAGLColorFormatRGBA8};
        self.multipleTouchEnabled = YES;
    }
    return self;
}
/* Touches get ids 0-9, the lowest free one, for as long as they last; the
 * game sees them as s3ePointer touch events (and touch 0 as the pointer). */
static UITouch *slots[10];
- (int)slotFor:(UITouch *)t make:(BOOL)make {
    for (int k = 0; k < 10; k++) if (slots[k] == t) return k;
    if (!make) return -1;
    for (int k = 0; k < 10; k++) if (!slots[k]) { slots[k] = t; return k; }
    return -1;
}
- (void)send:(NSSet<UITouch *> *)touches kind:(int)kind pressed:(int)pressed {
    CGSize s = self.bounds.size;
    if (s.width <= 0 || s.height <= 0) return;
    for (UITouch *t in touches) {
        int id = [self slotFor:t make:kind == 0 && pressed];
        if (id < 0) continue;
        CGPoint p = [t locationInView:self];
        int32_t x = (int32_t)(p.x * SCREEN_W / s.width), y = (int32_t)(p.y * SCREEN_H / s.height);
        if (getenv("RT_LOG_TOUCH")) rt_log("touch %d kind %d pressed %d at %d,%d", id, kind, pressed, x, y);
        if (kind == 0) input_touch(id, 1, 0, x, y);   /* move there first, as a mouse would */
        input_touch(id, kind, pressed, x, y);
        if (kind == 0 && !pressed) slots[id] = nil;
    }
}
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self send:touches kind:0 pressed:1];
}
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self send:touches kind:1 pressed:0];
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self send:touches kind:0 pressed:0];
}
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    [self send:touches kind:0 pressed:0];
}
@end

@interface GameViewController : UIViewController
@end

@implementation GameViewController
- (void)loadView {
    self.view = [[UIView alloc] init];
    self.view.backgroundColor = UIColor.blackColor;
    game_view = [[GameView alloc] initWithFrame:CGRectZero];
    [self.view addSubview:game_view];
}
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    /* the largest 3:2 rectangle, centred, with a 480x320 drawable */
    CGRect b = self.view.bounds;
    CGFloat w = b.size.width, h = w * SCREEN_H / SCREEN_W;
    if (h > b.size.height) { h = b.size.height; w = h * SCREEN_W / SCREEN_H; }
    game_view.frame = CGRectMake(floor((b.size.width - w) / 2), floor((b.size.height - h) / 2), w, h);
    game_view.contentScaleFactor = SCREEN_W / w;
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    start_game();
}
- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    return UIInterfaceOrientationMaskLandscape;
}
- (BOOL)prefersStatusBarHidden {
    return YES;
}
- (BOOL)prefersHomeIndicatorAutoHidden {
    return YES;
}
@end

/* ------------------------------------------------------- app and scene */
@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(strong, nonatomic) UIWindow *window;
@end

@implementation SceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session
      options:(UISceneConnectionOptions *)options {
    UIWindow *w = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    w.rootViewController = [[GameViewController alloc] init];
    [w makeKeyAndVisible];
    self.window = w;
}
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation AppDelegate
- (UISceneConfiguration *)application:(UIApplication *)application
    configurationForConnectingSceneSession:(UISceneSession *)session
                                   options:(UISceneConnectionOptions *)options {
    UISceneConfiguration *c = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
    c.delegateClass = SceneDelegate.class;
    return c;
}
@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(AppDelegate.class));
    }
}
