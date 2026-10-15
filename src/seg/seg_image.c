/* seg_image.c — 分割模型主机侧的尺寸账与 torch 式双线性缩放。
 * 输入侧的 PIL BILINEAR(抗锯齿)在 src/image/img_ops.c(img_resize_ch), 和改图工具共用一份;
 * 输出侧 seg_resize_f32 = torch F.interpolate(mode=bilinear, align_corners=False): 无抗锯齿, 源坐标 = (i+0.5)·in/out − 0.5 再夹到 ≥0。
 * 官方 postprocess_masks 用它把 256×256 的 logit 放大到 1024 再放到原图, 两步都是这个公式; 差一个核, 掩膜边缘就差一两个像素。 */
#include "ds4_seg.h"
#include <stdlib.h>

void seg_preprocess_shape(int w, int h, int *tw, int *th) {
    const double scale = (double)SEG_IMG / (w > h ? w : h);
    *tw = (int)(w * scale + 0.5); *th = (int)(h * scale + 0.5);
}

float *seg_resize_f32(const float *in, int w, int h, int tw, int th) {
    float *out = malloc(sizeof(float) * (size_t)tw * th);
    const double sx = (double)w / tw, sy = (double)h / th;
    for (int y = 0; y < th; y++) {
        double fy = (y + 0.5) * sy - 0.5; if (fy < 0) fy = 0;
        int y0 = (int)fy; if (y0 > h - 1) y0 = h - 1; const int y1 = y0 + 1 < h ? y0 + 1 : h - 1; const double ly = fy - y0;
        for (int x = 0; x < tw; x++) {
            double fx = (x + 0.5) * sx - 0.5; if (fx < 0) fx = 0;
            int x0 = (int)fx; if (x0 > w - 1) x0 = w - 1; const int x1 = x0 + 1 < w ? x0 + 1 : w - 1; const double lx = fx - x0;
            const double top = in[(size_t)y0 * w + x0] * (1 - lx) + in[(size_t)y0 * w + x1] * lx, bot = in[(size_t)y1 * w + x0] * (1 - lx) + in[(size_t)y1 * w + x1] * lx;
            out[(size_t)y * tw + x] = (float)(top * (1 - ly) + bot * ly);
        }
    }
    return out;
}
