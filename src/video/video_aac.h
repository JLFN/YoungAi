/* video_aac.h — AAC-LC 编码器内部: 规范表(video_aac_tables.c)。公开接口在 ds4_video.h。 */
#ifndef VIDEO_AAC_H
#define VIDEO_AAC_H
#include "video_internal.h"

extern const uint32_t va_sf_code[121];
extern const uint8_t va_sf_bits[121];
extern const uint16_t va_cb11_code[289];
extern const uint8_t va_cb11_bits[289];
extern const uint16_t va_swb_22k[48];

#endif
