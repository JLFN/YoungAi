/* vidcheck.m — 用 macOS 系统解码器(AVFoundation / VideoToolbox, 和 iPhone 同一套)解 mp4, 当视频编码器的判官。
 * 为什么不用 ffprobe: 产品不依赖 ffmpeg, 门也不依赖; 系统解码器是手机上真正播片的那一个, 也最挑。
 * 用法: vidcheck FILE.mp4 [REC.yuv [SRC.yuv]] [--audio SRC.pcm]
 *   只给 mp4: 打印帧数、尺寸、时长、帧率、关键帧数。
 *   给 REC(编码器重建, I420 显示尺寸逐帧拼接): 每帧 Y/Cb/Cr 逐字节比 —— 关了去块, 合法码流解出来必须一字不差。
 *   再给 SRC(编码器输入): 每帧 Y 的 PSNR(最低 / 平均)。
 *   有音轨就解成 22050 Hz 单声道 16 位(解不开 = FAIL); 给了 --audio 原声(raw 16 位小端单声道 22050 Hz)再比:
 *   在 ±4096 样本里找最佳对齐, 报相关系数、SNR、100 ms 能量包络相关。门: 对齐偏差 ≤ 64 样本、相关 ≥ 0.95、包络相关 ≥ 0.95、长度差 ≤ 2048。
 * 输出最后一行: "vidcheck: OK …" 或 "vidcheck: FAIL …", 退出码 0 / 1。 */
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#include <math.h>
#include <stdio.h>

static int check_audio(AVURLAsset *asset, const char *srcpath) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    NSArray *at = [asset tracksWithMediaType:AVMediaTypeAudio];
#pragma clang diagnostic pop
    if (!at.count) { if (srcpath) { printf("audio: FAIL no audio track\n"); return 1; } return 0; }
    NSError *err = nil;
    AVAssetReader *r = [AVAssetReader assetReaderWithAsset:asset error:&err];
    NSDictionary *as = @{ AVFormatIDKey: @(kAudioFormatLinearPCM), AVLinearPCMBitDepthKey: @16, AVLinearPCMIsFloatKey: @NO, AVLinearPCMIsBigEndianKey: @NO,
                          AVLinearPCMIsNonInterleaved: @NO, AVSampleRateKey: @22050, AVNumberOfChannelsKey: @1 };
    AVAssetReaderTrackOutput *o = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:at[0] outputSettings:as];
    [r addOutput:o];
    if (![r startReading]) { printf("audio: FAIL startReading: %s\n", r.error.localizedDescription.UTF8String); return 1; }
    int16_t *a = NULL; size_t n = 0, cap = 0; CMSampleBufferRef sb;
    while ((sb = [o copyNextSampleBuffer])) {
        CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
        const size_t len = bb ? CMBlockBufferGetDataLength(bb) : 0;
        if (n + len / 2 > cap) { cap = (n + len / 2) * 2; a = realloc(a, cap * 2); }
        if (len) CMBlockBufferCopyDataBytes(bb, 0, len, a + n);
        n += len / 2; CFRelease(sb);
    }
    if (r.status == AVAssetReaderStatusFailed) { printf("audio: FAIL decode error: %s\n", r.error.localizedDescription.UTF8String); return 1; }
    double e = 0; for (size_t i = 0; i < n; i++) e += (double)a[i] * a[i];
    printf("audio: decoded %zu samples (%.3f s), rms %.1f\n", n, n / 22050.0, n ? sqrt(e / n) : 0);
    if (!srcpath) { free(a); return n > 0 ? 0 : 1; }
    FILE *f = fopen(srcpath, "rb"); if (!f) { printf("audio: FAIL cannot read %s\n", srcpath); return 1; }
    fseek(f, 0, SEEK_END); const size_t ns = (size_t)ftell(f) / 2; fseek(f, 0, SEEK_SET);
    int16_t *s = malloc(ns * 2 + 2); if (fread(s, 2, ns, f) != ns) { } fclose(f);
    const long m = (long)(n < ns ? n : ns);
    long best = 0; double bc = -2;
    for (long lag = -4096; lag <= 4096; lag++) {   /* a[i + lag] 对 s[i] */
        double xy = 0, xx = 0, yy = 0;
        for (long i = 0; i < m; i += 4) { const long j = i + lag; if (j < 0 || j >= (long)n) continue; xy += (double)a[j] * s[i]; xx += (double)a[j] * a[j]; yy += (double)s[i] * s[i]; }
        const double c = xx > 0 && yy > 0 ? xy / sqrt(xx * yy) : 0;
        if (c > bc) { bc = c; best = lag; }
    }
    double xy = 0, xx = 0, yy = 0, ee = 0;
    for (long i = 0; i < m; i++) { const long j = i + best; if (j < 0 || j >= (long)n) continue; const double x = a[j], y = s[i]; xy += x * y; xx += x * x; yy += y * y; ee += (x - y) * (x - y); }
    const double corr = xy / sqrt(xx * yy + 1e-9), snr = 10 * log10(yy / (ee + 1e-9));
    const long W = 2205, nw = m / W; double *ea = calloc((size_t)nw + 1, sizeof *ea), *es = calloc((size_t)nw + 1, sizeof *es);
    for (long w = 0; w < nw; w++) for (long i = w * W; i < (w + 1) * W; i++) { const long j = i + best; if (j >= 0 && j < (long)n) ea[w] += (double)a[j] * a[j]; es[w] += (double)s[i] * s[i]; }
    double mx = 0, my = 0; for (long w = 0; w < nw; w++) { ea[w] = sqrt(ea[w] / W); es[w] = sqrt(es[w] / W); mx += ea[w]; my += es[w]; }
    mx /= nw ? nw : 1; my /= nw ? nw : 1;
    double sxy = 0, sxx = 0, syy = 0; for (long w = 0; w < nw; w++) { sxy += (ea[w] - mx) * (es[w] - my); sxx += (ea[w] - mx) * (ea[w] - mx); syy += (es[w] - my) * (es[w] - my); }
    const double env = sxy / sqrt(sxx * syy + 1e-9);
    const long dlen = (long)n - (long)ns;
    const bool ok = labs(best) <= 64 && corr >= 0.95 && env >= 0.95 && labs(dlen) <= 2048;
    printf("audio: %s lag %ld samples, waveform corr %.4f, SNR %.1f dB, 100 ms envelope corr %.4f, decoded %zu vs source %zu samples\n", ok ? "OK" : "FAIL", best, corr, snr, env, n, ns);
    free(a); free(s); free(ea); free(es);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        const char *apath = NULL;
        for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--audio")) { apath = argv[i + 1]; for (int k = i; k + 2 <= argc; k++) argv[k] = argv[k + 2]; argc -= 2; break; }
        if (argc < 2) { fprintf(stderr, "usage: vidcheck FILE.mp4 [REC.yuv [SRC.yuv]] [--audio SRC.pcm]\n"); return 2; }
        NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]];
        AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:nil];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        NSArray *tracks = [asset tracksWithMediaType:AVMediaTypeVideo];
        if (!tracks.count) {   /* 纯音频文件(拿苹果自己编的 m4a 对照预滚口径时用): 只查音轨 */
            const int aud = check_audio(asset, apath);
            printf("vidcheck: %s audio-only file\n", aud ? "FAIL" : "OK"); return aud ? 1 : 0;
        }
        AVAssetTrack *t = tracks[0];
        const CGSize sz = t.naturalSize; const double dur = CMTimeGetSeconds(asset.duration); const float fps = t.nominalFrameRate;
#pragma clang diagnostic pop
        const int w = (int)sz.width, h = (int)sz.height;
        printf("track: %dx%d, %.3f s, %.2f fps\n", w, h, dur, fps);
        NSError *err = nil;
        AVAssetReader *r = [AVAssetReader assetReaderWithAsset:asset error:&err];
        if (!r) { printf("vidcheck: FAIL reader: %s\n", err.localizedDescription.UTF8String); return 1; }
        NSDictionary *set = @{ (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) };   /* 解码器原生输出, 不做色彩转换 */
        AVAssetReaderTrackOutput *o = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:t outputSettings:set];
        o.alwaysCopiesSampleData = NO;
        [r addOutput:o];
        if (![r startReading]) { printf("vidcheck: FAIL startReading: %s\n", r.error.localizedDescription.UTF8String); return 1; }
        long keys = 0, samples = 0;   /* 关键帧要读压缩样本数: 解码后的帧不带 NotSync 标记 */
        {
            AVAssetReader *rk = [AVAssetReader assetReaderWithAsset:asset error:&err];
            AVAssetReaderTrackOutput *ok = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:t outputSettings:nil];
            [rk addOutput:ok]; [rk startReading];
            CMSampleBufferRef s;
            while ((s = [ok copyNextSampleBuffer])) {
                CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(s, false);
                if (CMSampleBufferGetNumSamples(s) > 0) { samples++; if (!att || !CFArrayGetCount(att) || !CFDictionaryContainsKey(CFArrayGetValueAtIndex(att, 0), kCMSampleAttachmentKey_NotSync)) keys++; }
                CFRelease(s);
            }
        }
        FILE *frec = argc > 2 ? fopen(argv[2], "rb") : NULL, *fsrc = argc > 3 ? fopen(argv[3], "rb") : NULL;
        const size_t fsz = (size_t)w * h * 3 / 2;
        uint8_t *rec = malloc(fsz), *src = malloc(fsz);
        long frames = 0, exact = 0; int worst = 0; double pmin = 1e9, psum = 0;
        CMSampleBufferRef sb;
        while ((sb = [o copyNextSampleBuffer])) {
            CVImageBufferRef ib = CMSampleBufferGetImageBuffer(sb);
            if (!ib) { CFRelease(sb); continue; }
            CVPixelBufferLockBaseAddress(ib, kCVPixelBufferLock_ReadOnly);
            const uint8_t *Y = CVPixelBufferGetBaseAddressOfPlane(ib, 0), *C = CVPixelBufferGetBaseAddressOfPlane(ib, 1);
            const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(ib, 0), cs = CVPixelBufferGetBytesPerRowOfPlane(ib, 1);
            const int pw = (int)CVPixelBufferGetWidth(ib), ph = (int)CVPixelBufferGetHeight(ib);
            if (pw != w || ph != h) { printf("frame %ld: decoded %dx%d != %dx%d\n", frames, pw, ph, w, h); worst = 255; }
            else if (frec && fread(rec, 1, fsz, frec) == fsz) {
                int d = 0;
                for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { const int e = abs(Y[y * ys + x] - rec[(size_t)y * w + x]); if (e > d) d = e; }
                const uint8_t *U = rec + (size_t)w * h, *V = U + (size_t)w * h / 4;
                for (int y = 0; y < h / 2; y++) for (int x = 0; x < w / 2; x++) {
                    const int eu = abs(C[y * cs + 2 * x] - U[y * (w / 2) + x]), ev = abs(C[y * cs + 2 * x + 1] - V[y * (w / 2) + x]);
                    if (eu > d) d = eu; if (ev > d) d = ev;
                }
                if (!d) exact++; else if (d > worst || exact == frames) printf("frame %ld: max |decoded - recon| = %d\n", frames, d);
                if (d > worst) worst = d;
                if (fsrc && fread(src, 1, fsz, fsrc) == fsz) {
                    double se = 0;
                    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { const int e = Y[y * ys + x] - src[(size_t)y * w + x]; se += e * e; }
                    const double p = se ? 10 * log10(255.0 * 255.0 * w * h / se) : 99; if (p < pmin) pmin = p; psum += p;
                }
            }
            CVPixelBufferUnlockBaseAddress(ib, kCVPixelBufferLock_ReadOnly);
            CFRelease(sb); frames++;
        }
        if (r.status == AVAssetReaderStatusFailed) { printf("vidcheck: FAIL decode error after %ld frames: %s\n", frames, r.error.localizedDescription.UTF8String); return 1; }
        printf("decoded %ld frames; %ld samples, %ld key\n", frames, samples, keys);
        if (fsrc && frames) printf("Y-PSNR vs source: min %.2f dB, mean %.2f dB\n", pmin, psum / frames);
        const int aud = check_audio(asset, apath);
        const bool ok = frames > 0 && frames == samples && (!frec || (exact == frames && worst == 0)) && aud == 0;
        if (frec) printf("vidcheck: %s %ld/%ld frames bit-exact with encoder recon (worst diff %d)\n", ok ? "OK" : "FAIL", exact, frames, worst);
        else printf("vidcheck: %s %ld frames\n", ok ? "OK" : "FAIL", frames);
        return ok ? 0 : 1;
    }
}
