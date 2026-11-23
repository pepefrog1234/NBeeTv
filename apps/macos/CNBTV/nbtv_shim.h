/*
 * Swift 外殼用的薄墊片：匯入核心的 C API，並補上幾個 Swift 不方便直接做的小函式。
 *
 * - nbtv_rx_frame 裡有很大的固定長度陣列（rgb 20736 位元組），Swift 會把 C 陣列匯入成 tuple，
 *   太大的匯不進來，所以用函式取指標。
 * - 序列埠的 RTS／DTR 要用 ioctl 的巨集常數，Swift 讀不到。
 */
#ifndef NBTV_SHIM_H
#define NBTV_SHIM_H

#include <stdlib.h>
#include <sys/ioctl.h>

#include "../../../core/include/nbtv/nbtv.h"

static inline nbtv_rx_frame *nbtv_shim_frame_alloc(void) { return (nbtv_rx_frame *)calloc(1, sizeof(nbtv_rx_frame)); }
static inline void nbtv_shim_frame_free(nbtv_rx_frame *frame) { free(frame); }
static inline const uint8_t *nbtv_shim_frame_rgb(const nbtv_rx_frame *frame) { return frame->rgb; }
static inline const char *nbtv_shim_frame_callsign(const nbtv_rx_frame *frame) { return frame->callsign; }

/* 序列埠的數據機控制線：line 是 0（RTS）或 1（DTR）。成功回傳 0。 */
static inline int nbtv_shim_set_modem_line(int fd, int line, int on) {
    int bits = line == 0 ? TIOCM_RTS : TIOCM_DTR;
    return ioctl(fd, on ? TIOCMBIS : TIOCMBIC, &bits);
}

#endif /* NBTV_SHIM_H */
