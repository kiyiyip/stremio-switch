#ifndef HTTPSTREAM_H
#define HTTPSTREAM_H

#include <mpv/client.h>

// Registers the "debrid" mpv protocol, backed by libcurl with byte-range
// requests (https included -- devkitPro's ffmpeg has no https protocol, so
// mpv cannot open a direct URL itself). `url` must outlive the mpv instance.
//
// Returns 0 on success.
int httpstream_register(mpv_handle *mpv, const char *url);

#endif
