// Frame generation on a real capture: LARECOMP_FRAMEGEN_SHOT's prev | generated | cur strip in, the passes
// (runtime/nvn/framegen_src.c through framegen_metal.m) run again on its prev and cur, prev | new | cur out - to tune
// the passes on the game's own frames without the game. Built by `make build/framegen_png`.
//   framegen_png <strip.png> <out.png> [t]
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "nvn/framegen.h"

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static uint8_t *load_rgba(const char *path, int *w, int *h) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageSourceRef src = CGImageSourceCreateWithURL(url, NULL);
    CFRelease(url);
    if (!src) return NULL;
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, NULL);
    CFRelease(src);
    if (!img) return NULL;
    *w = (int)CGImageGetWidth(img), *h = (int)CGImageGetHeight(img);
    uint8_t *px = calloc((size_t)*w * *h, 4);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(px, (size_t)*w, (size_t)*h, 8, (size_t)*w * 4, cs,
                                             kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGContextDrawImage(ctx, CGRectMake(0, 0, *w, *h), img);
    CGContextRelease(ctx), CGColorSpaceRelease(cs), CGImageRelease(img);
    return px;
}

static bool save_rgba(const char *path, const uint8_t *px, int w, int h) {
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate((void *)px, (size_t)w, (size_t)h, 8, (size_t)w * 4, cs,
                                             kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL);
    bool ok = dst != NULL;
    if (dst) CGImageDestinationAddImage(dst, img, NULL), ok = CGImageDestinationFinalize(dst), CFRelease(dst);
    CFRelease(url), CGImageRelease(img), CGContextRelease(ctx), CGColorSpaceRelease(cs);
    return ok;
}

static id<MTLTexture> texture(id<MTLDevice> dev, int w, int h, const uint8_t *px, int stride) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:(NSUInteger)w
                                                                                 height:(NSUInteger)h
                                                                              mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [dev newTextureWithDescriptor:td];
    if (px) [t replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h) mipmapLevel:0 withBytes:px bytesPerRow:(NSUInteger)stride];
    return t;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <prev|gen|cur strip.png> <out.png> [t]\n", argv[0]);
        return 2;
    }
    float t = argc > 3 ? (float)atof(argv[3]) : 0.5f;
    int sw, h;
    uint8_t *strip = load_rgba(argv[1], &sw, &h);
    if (!strip) { fprintf(stderr, "can't read %s\n", argv[1]); return 1; }
    int w = sw / 3;
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLTexture> prev = texture(dev, w, h, strip, sw * 4), cur = texture(dev, w, h, strip + (size_t)2 * w * 4, sw * 4);
    id<MTLTexture> out = texture(dev, w, h, NULL, 0);
    id<MTLCommandBuffer> cb = [q commandBuffer];
    if (!fgm_encode(dev, cb, prev, cur, t, true, out)) { fprintf(stderr, "fgm_encode failed\n"); return 1; }
    [cb commit];
    [cb waitUntilCompleted];
    uint8_t *gen = malloc((size_t)w * h * 4);
    [out getBytes:gen bytesPerRow:(NSUInteger)w * 4 fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h) mipmapLevel:0];
    for (int y = 0; y < h; y++) memcpy(strip + ((size_t)y * sw + w) * 4, gen + (size_t)y * w * 4, (size_t)w * 4);
    if (!save_rgba(argv[2], strip, sw, h)) { fprintf(stderr, "can't write %s\n", argv[2]); return 1; }
    return 0;
}
