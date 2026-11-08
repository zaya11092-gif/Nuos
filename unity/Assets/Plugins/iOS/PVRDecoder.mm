// NuOS v2 — HEVC decoder plugin for Unity (iOS).
// Feeds Annex-B HEVC AUs into VideoToolbox and exposes each decoded frame's
// plane-0 pointer as a Metal texture for Unity's CreateExternalTexture flow.
//
// C API (called from C# via DllImport("__Internal")):
//   void*  pvrdec_create(int width, int height);
//   int    pvrdec_params(void* h, const char* vps, int vl, const char* sps,
//                        int sl, const char* pps, int pl);   // 0 = ok
//   int    pvrdec_feed(void* h, const char* au, int len);      // 1 = frame out
//   void*  pvrdec_texture(void* h, int* w, int* h2);           // MTLTexture
//   void   pvrdec_destroy(void* h);
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <Metal/Metal.h>

static void PvrDecLog(NSString* fmt, ...) {}

@interface PVRDecoder : NSObject
@property VTDecompressionSessionRef session;
@property CMVideoFormatDescriptionRef desc;
@property CVMetalTextureCacheRef texCache;
@property id<MTLTexture> lastTexture;
@property int width, height;
@property BOOL failed;
@end

static void PVRDecoderOutputCallback(void* refCon, void* decompressionOut,
                                     OSStatus status, VTDecodeInfoFlags info,
                                     CVImageBufferRef image, CMTime pts,
                                     CMTime dts) {
    if (status != noErr || !image) return;
    PVRDecoder* self = (__bridge PVRDecoder*)refCon;
    if (@available(iOS 11.0, *))
        CVMetalTextureCacheFlush(self.texCache, 0);
    CVMetalTextureRef tex = nullptr;
    OSStatus st = CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault, self.texCache, image, nullptr,
        MTLPixelFormatBGRA8Unorm, self.width, self.height, 0, &tex);
    if (st != kCVReturnSuccess || !tex) return;
    CVMetalTextureRef t = tex;
    self.lastTexture = CVMetalTextureGetTexture(t);
    CVBufferRelease(t);
}

static NSArray* SplitAnnexB(NSData* data) {
    // Handles both 3-byte (00 00 01) and 4-byte (00 00 00 01) start codes.
    NSMutableArray* out = [NSMutableArray array];
    const uint8_t* d = (const uint8_t*)data.bytes;
    NSUInteger n = data.length;
    NSUInteger i = 0, nalStart = 0;
    BOOL inNal = NO;
    while (i + 3 <= n) {
        BOOL sc3 = (d[i] == 0 && d[i+1] == 0 && d[i+2] == 1);
        BOOL sc4 = (i + 4 <= n && d[i] == 0 && d[i+1] == 0 && d[i+2] == 0 && d[i+3] == 1);
        if (sc3 || sc4) {
            if (inNal) [out addObject:[data subdataWithRange:NSMakeRange(nalStart, i - nalStart)]];
            i += sc4 ? 4 : 3;
            nalStart = i;
            inNal = YES;
        } else {
            i++;
        }
    }
    if (inNal) [out addObject:[data subdataWithRange:NSMakeRange(nalStart, n - nalStart)]];
    return out;
}

@implementation PVRDecoder
@end

extern "C" {

void* pvrdec_create(int width, int height) {
    PVRDecoder* d = [PVRDecoder new];
    d.width = width; d.height = height;
    CVMetalTextureCacheRef cache = nullptr;
    CVReturn r = CVMetalTextureCacheCreate(
        kCFAllocatorDefault, nullptr,
        (id<MTLDevice>)MTLCopyAllDevices().firstObject, nullptr, &cache);
    d.texCache = cache;
    if (r != kCVReturnSuccess) { PvrDecLog(@"texcache fail"); return nullptr; }
    return (__bridge_retained void*)d;
}

int pvrdec_params(void* h, const char* vps, int vl, const char* sps, int sl,
                  const char* pps, int pl) {
    PVRDecoder* d = (__bridge PVRDecoder*)h;
    const uint8_t* ptrs[3] = {(const uint8_t*)vps, (const uint8_t*)sps,
                              (const uint8_t*)pps};
    const size_t sizes[3] = {(size_t)vl, (size_t)sl, (size_t)pl};
    CMVideoFormatDescriptionRef descOut = nullptr;
    OSStatus st = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
        kCFAllocatorDefault, 3, ptrs, sizes, 4, nullptr, &descOut);
    d.desc = descOut;
    if (st != noErr) { d.failed = YES; return (int)st; }

    NSDictionary* spec = @{
        (NSString*)kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder:
            @YES,
    };
    NSDictionary* dst = @{
        (NSString*)kCVPixelBufferPixelFormatTypeKey:
            @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (NSString*)kCVPixelBufferMetalCompatibilityKey: @YES,
        (NSString*)kCVPixelBufferWidthKey: @(d.width),
        (NSString*)kCVPixelBufferHeightKey: @(d.height),
    };
    VTDecompressionOutputCallbackRecord record;
    record.decompressionOutputCallback = PVRDecoderOutputCallback;
    record.decompressionOutputRefCon = (__bridge void*)d;
    VTDecompressionSessionRef session;
    st = VTDecompressionSessionCreate(
        kCFAllocatorDefault, d.desc, (__bridge CFDictionaryRef)spec,
        (__bridge CFDictionaryRef)dst, &record, &session);
    if (st != noErr) { d.failed = YES; return (int)st; }
    d.session = session;
    return 0;
}

int pvrdec_feed(void* h, const char* au, int len) {
    PVRDecoder* d = (__bridge PVRDecoder*)h;
    if (d.failed || !d.session || len <= 0) return -1;

    NSData* annexB = [NSData dataWithBytes:au length:len];
    NSArray* nals = SplitAnnexB(annexB);
    NSMutableData* prefixed = [NSMutableData data];
    for (NSData* nal in nals) {
        uint32_t n = (uint32_t)nal.length;
        uint8_t b[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16),
                        (uint8_t)(n >> 8), (uint8_t)n};
        [prefixed appendBytes:b length:4];
        [prefixed appendData:nal];
    }

    CMBlockBufferRef block = nullptr;
    OSStatus st = CMBlockBufferCreateWithMemoryBlock(
        kCFAllocatorDefault, nullptr, prefixed.length, nullptr, nullptr, 0,
        prefixed.length, 0, &block);
    if (st != kCMBlockBufferNoErr) return -1;
    CMBlockBufferReplaceDataBytes(prefixed.bytes, block, 0, prefixed.length);

    CMSampleBufferRef sample = nullptr;
    CMVideoFormatDescriptionRef desc = d.desc;
    NSUInteger auLen = prefixed.length;
    st = CMSampleBufferCreateReady(kCFAllocatorDefault, block, desc, 1, 0,
                                   nullptr, 1, &auLen, &sample);
    if (st != noErr) { CFRelease(block); return -1; }

    st = VTDecompressionSessionDecodeFrame(d.session, sample,
        kVTDecodeFrame_EnableAsynchronousDecompression, nullptr, nullptr);
    CFRelease(sample);
    CFRelease(block);
    return (st == noErr) ? 1 : (int)st;
}

void* pvrdec_texture(void* h, int* w, int* h2) {
    PVRDecoder* d = (__bridge PVRDecoder*)h;
    if (!d.lastTexture) return nullptr;
    *w = (int)d.lastTexture.width;
    *h2 = (int)d.lastTexture.height;
    return (__bridge void*)d.lastTexture;
}

void pvrdec_destroy(void* h) {
    PVRDecoder* d = (__bridge_transfer PVRDecoder*)h;
    if (d.session) VTDecompressionSessionInvalidate(d.session);
    d.session = nullptr;
}

}  // extern "C"
