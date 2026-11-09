#include "httpstream.h"

#include <curl/curl.h>
#include <mpv/stream_cb.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// One ranged request per chunk. mpv reads in ~32 KiB pieces, so a few MiB
// means one request per few seconds of video instead of one per read, and the
// first request is small enough that a slow link does not stall the open for
// long.
#define CHUNK (4 * 1024 * 1024)
// Hard ceiling: a server that ignores Range would otherwise stream a whole
// movie into RAM.
#define MAX_BUF (16 * 1024 * 1024)

typedef struct
{
    const char *url;
    CURL *curl;
    pthread_mutex_t lock;

    int64_t pos;      // next byte the caller wants
    int64_t bufStart; // file offset of buf[0]
    int64_t bufLen;   // valid bytes in buf
    int64_t bufCap;
    unsigned char *buf;

    int64_t size;       // total stream size, -1 until known
    int64_t contentLen; // Content-Length of the current response
    int failed;
} hctx;

static size_t write_cb(char *p, size_t sz, size_t n, void *ud)
{
    hctx *s      = ud;
    size_t len   = sz * n;
    int64_t room = s->bufCap - s->bufLen;
    if ((int64_t)len > room)
    {
        // Stop reading: more than this response should have been.
        memcpy(s->buf + s->bufLen, p, (size_t)room);
        s->bufLen += room;
        return 0; // aborts the transfer on purpose (CURLE_WRITE_ERROR)
    }
    memcpy(s->buf + s->bufLen, p, len);
    s->bufLen += len;
    return len;
}

// "Content-Range: bytes 0-4194303/1234567890" carries the total size, which
// the player needs for seeking (mp4 with the index at the end, in particular).
static size_t header_cb(char *p, size_t sz, size_t n, void *ud)
{
    hctx *s    = ud;
    size_t len = sz * n;
    if (len > 14 && !strncasecmp(p, "content-range:", 14))
    {
        const char *slash = memchr(p, '/', len);
        if (slash && slash + 1 < p + len)
        {
            long long v = atoll(slash + 1);
            if (v > 0) s->size = v;
        }
    }
    else if (len > 15 && !strncasecmp(p, "content-length:", 15))
    {
        long long v = atoll(p + 15);
        if (v > 0) s->contentLen = v;
    }
    return len;
}

// Fetches up to `want` bytes from `pos` into the buffer. Returns bytes fetched,
// 0 at EOF, -1 on error.
static int fetch(hctx *s, int64_t pos, int64_t want)
{
    if (want < CHUNK) want = CHUNK;
    if (want > MAX_BUF) want = MAX_BUF;

    s->bufStart   = pos;
    s->bufLen     = 0;
    s->contentLen = 0;

    if (s->size >= 0 && pos >= s->size) return 0; // past the end

    if (s->bufCap < want)
    {
        unsigned char *nb = realloc(s->buf, (size_t)want);
        if (!nb) return -1;
        s->buf    = nb;
        s->bufCap = want;
    }

    int64_t end = pos + want - 1;
    if (s->size >= 0 && end >= s->size) end = s->size - 1;

    char range[64];
    snprintf(range, sizeof(range), "%lld-%lld", (long long)pos, (long long)end);
    curl_easy_setopt(s->curl, CURLOPT_RANGE, range);

    CURLcode rc = curl_easy_perform(s->curl);
    long code   = 0;
    curl_easy_getinfo(s->curl, CURLINFO_RESPONSE_CODE, &code);

    if (code == 416) return 0; // past the end
    // The cap aborted the transfer after a full buffer: that is success.
    if (rc == CURLE_WRITE_ERROR && s->bufLen == s->bufCap) rc = CURLE_OK;
    if (rc != CURLE_OK) return -1;
    if (code != 200 && code != 206) return -1;
    // 200 to a ranged request means the server ignored Range. Fine at offset
    // 0 (Content-Length is the total); anywhere else there is no way to reach
    // the requested bytes.
    if (code == 200 && pos > 0) return -1;
    if (code == 200 && s->contentLen > 0) s->size = s->contentLen;

    if (s->size >= 0 && pos + s->bufLen > s->size) s->bufLen = s->size - pos;
    return (int)s->bufLen;
}

static int64_t read_cb(void *cookie, char *buf, uint64_t nbytes)
{
    hctx *s = cookie;
    if (nbytes > MAX_BUF) nbytes = MAX_BUF;

    pthread_mutex_lock(&s->lock);
    if (s->failed)
    {
        pthread_mutex_unlock(&s->lock);
        return -1;
    }

    if (s->pos < s->bufStart || s->pos >= s->bufStart + s->bufLen)
    {
        if (fetch(s, s->pos, (int64_t)nbytes) < 0)
        {
            s->failed = 1;
            pthread_mutex_unlock(&s->lock);
            return -1;
        }
        if (s->bufLen == 0)
        {
            pthread_mutex_unlock(&s->lock);
            return 0; // EOF
        }
    }

    int64_t off   = s->pos - s->bufStart;
    int64_t avail = s->bufLen - off;
    int64_t n     = (int64_t)nbytes < avail ? (int64_t)nbytes : avail;
    memcpy(buf, s->buf + off, (size_t)n);
    s->pos += n;
    pthread_mutex_unlock(&s->lock);
    return n;
}

static int64_t seek_cb(void *cookie, int64_t offset)
{
    if (offset < 0) return MPV_ERROR_GENERIC;
    hctx *s = cookie;
    pthread_mutex_lock(&s->lock);
    s->pos = offset;
    pthread_mutex_unlock(&s->lock);
    return offset;
}

static int64_t size_cb(void *cookie)
{
    hctx *s = cookie;
    return s->size >= 0 ? s->size : MPV_ERROR_UNSUPPORTED;
}

static void free_ctx(hctx *s)
{
    if (s->curl) curl_easy_cleanup(s->curl);
    pthread_mutex_destroy(&s->lock);
    free(s->buf);
    free(s);
}

static void close_cb(void *cookie)
{
    free_ctx(cookie);
}

static int open_cb(void *user_data, char *uri, mpv_stream_cb_info *info)
{
    (void)uri; // the URL comes from user_data; see httpstream_register
    hctx *s = calloc(1, sizeof(*s));
    if (!s) return MPV_ERROR_LOADING_FAILED;

    s->url  = user_data;
    s->size = -1;
    pthread_mutex_init(&s->lock, NULL);

    s->curl = curl_easy_init();
    if (!s->curl)
    {
        free_ctx(s);
        return MPV_ERROR_LOADING_FAILED;
    }

    curl_easy_setopt(s->curl, CURLOPT_URL, s->url);
    curl_easy_setopt(s->curl, CURLOPT_USERAGENT, "stremio-switch");
    curl_easy_setopt(s->curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(s->curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(s->curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(s->curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(s->curl, CURLOPT_LOW_SPEED_TIME, 30L);
    // Debrid links are usually https; same CA store the API calls use (the
    // console mounts none, so it ships in the nro's romfs).
#ifdef __SWITCH__
    curl_easy_setopt(s->curl, CURLOPT_CAINFO, "romfs:/cacert.pem");
#endif
    curl_easy_setopt(s->curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(s->curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(s->curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(s->curl, CURLOPT_WRITEDATA, s);
    curl_easy_setopt(s->curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(s->curl, CURLOPT_HEADERDATA, s);

    // Probe now: this delivers the first chunk and the total size, so opening
    // (and seeking) works before the first read.
    if (fetch(s, 0, CHUNK) < 0)
    {
        free_ctx(s);
        return MPV_ERROR_LOADING_FAILED;
    }

    info->cookie   = s;
    info->read_fn  = read_cb;
    info->seek_fn  = seek_cb;
    info->size_fn  = size_cb;
    info->close_fn = close_cb;
    return 0;
}

int httpstream_register(mpv_handle *mpv, const char *url)
{
    return mpv_stream_cb_add_ro(mpv, "debrid", (void *)url, open_cb);
}
