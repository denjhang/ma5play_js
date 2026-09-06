/* ma5play_shell.c — ma5t WASM 壳层（libma5t_compact 版 = GUI ma5t 后端同源）
 * 渲染语义照抄 ma5t_player.c 的 ma5p_pump：
 *   no_pcm_iters > 100（round19：melody11 有 >5 泵断流，5 会误杀）；
 *   出声后连续 2 秒全零 → 曲终并丢弃尾部静音。
 * 生命周期同 GUI：一次 init 一次 load，切曲 = shutdown + 重新 init。 */
#include <emscripten.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* compact host 的 DLL 磁盘查找路径桩（wasm 走内嵌 DLL，永不命中） */
unsigned long GetModuleFileNameA(void *h, char *buf, unsigned long n) {
    (void)h; (void)buf; (void)n; return 0;
}

typedef struct ma5t_ctx ma5t_ctx_t;
extern int  ma5t_init(const char *dir, ma5t_ctx_t **out);
extern int  ma5t_load(ma5t_ctx_t *, const void *, uint32_t);
extern int  ma5t_open_standby_start(ma5t_ctx_t *);
extern void ma5t_pump_seq(ma5t_ctx_t *);
extern void ma5t_pump_audio(ma5t_ctx_t *);
extern uint32_t ma5t_take_pcm(ma5t_ctx_t *, uint8_t *, uint32_t);
extern const char *ma5t_last_error(void);
extern void ma5t_set_preload_image(const unsigned char *img, unsigned int size);
extern void ma5t_shutdown(ma5t_ctx_t *c);
extern int  ma5t_pause(ma5t_ctx_t *c);
extern int  ma5t_resume(ma5t_ctx_t *c);
extern int  ma5t_seek(ma5t_ctx_t *c, int pos_ms);
extern int  ma5t_start_play(ma5t_ctx_t *c, int loops);
extern int g_compact_mode;

/* ma5p_ctx 等价状态 */
static ma5t_ctx_t *g_t = NULL;
static int music_seen, no_pcm_iters, ended, paused;
static uint32_t silent_bytes, played_bytes;
static void *g_mmf = NULL;
static char g_dll_dir[512] = { 0 };        /* load 时重建引擎用的 DLL 目录 */

EMSCRIPTEN_KEEPALIVE
void ma5w_set_preload(const unsigned char *img, unsigned int size) {
    ma5t_set_preload_image(img, size);
}

EMSCRIPTEN_KEEPALIVE
int ma5w_init(const char *dir) {           /* dir = DLL 搜索目录（web: NULL 用 MEMFS cwd） */
    g_t = NULL;
    music_seen = no_pcm_iters = ended = paused = 0;
    silent_bytes = played_bytes = 0;
    if (dir) { strncpy(g_dll_dir, dir, sizeof(g_dll_dir) - 1); g_dll_dir[sizeof(g_dll_dir) - 1] = 0; }
    return ma5t_init(dir, &g_t);
}

EMSCRIPTEN_KEEPALIVE
int ma5w_load(const void *mmf, uint32_t size) {
    if (g_t) { ma5t_shutdown(g_t); g_t = NULL; }   /* 切曲：重建引擎 */
    if (ma5t_init(g_dll_dir[0] ? g_dll_dir : NULL, &g_t) != 0) return -1;
    music_seen = no_pcm_iters = ended = paused = 0;
    silent_bytes = played_bytes = 0;
    free(g_mmf);
    g_mmf = malloc(size);
    if (!g_mmf) return -1;
    memcpy(g_mmf, mmf, size);
    if (ma5t_load(g_t, g_mmf, size) != 0) return -1;
    return ma5t_open_standby_start(g_t);
}

EMSCRIPTEN_KEEPALIVE
int ma5w_pump(int16_t *dst, int max_frames) {
    if (!g_t || !dst || max_frames <= 0) return -1;
    if (ended || paused) return 0;
    ma5t_pump_seq(g_t);
    ma5t_pump_audio(g_t);
    uint32_t want = (uint32_t)max_frames * 4u;
    uint32_t n = ma5t_take_pcm(g_t, (uint8_t *)dst, want);
    if (n == 0) {
        if (music_seen && ++no_pcm_iters > 100) { ended = 1; return 0; }
        return 0;
    }
    no_pcm_iters = 0;
    int nz = 0;
    for (uint32_t i = 0; i + 1 < n; i += 2)
        if (dst[i >> 1]) { nz = 1; break; }
    if (nz) { music_seen = 1; silent_bytes = 0; }
    else if (music_seen) silent_bytes += n;
    played_bytes += n;
    if (music_seen && silent_bytes > (uint32_t)(2.0 * 192000.0)) {
        uint32_t drop = silent_bytes;
        uint32_t keep = (n >= drop) ? n - drop : 0;
        played_bytes -= drop;
        silent_bytes = 0;
        ended = 1;
        return (int)(keep / 4u);
    }
    return (int)(n / 4u);
}

EMSCRIPTEN_KEEPALIVE
int ma5w_pause(void)  { if (!g_t) return -1; paused = 1; return ma5t_pause(g_t); }
EMSCRIPTEN_KEEPALIVE
int ma5w_resume(void) {
    if (!g_t) return -1;
    int r = ma5t_resume(g_t);
    paused = 0; no_pcm_iters = 0;   /* 暂停期间 ring 被抽干，恢复后别误判曲终 */
    return r;
}
EMSCRIPTEN_KEEPALIVE
int ma5w_seek_play(int pos_ms) {
    if (!g_t) return -1;
    int r = ma5t_seek(g_t, pos_ms);
    if (r != 0) return r;
    r = ma5t_start_play(g_t, 1);
    ended = 0; no_pcm_iters = 0; silent_bytes = 0; paused = 0;
    return r;
}

EMSCRIPTEN_KEEPALIVE
int ma5w_ended(void) { return ended; }
EMSCRIPTEN_KEEPALIVE
uint32_t ma5w_played_ms(void) { return played_bytes / 192u; }
EMSCRIPTEN_KEEPALIVE
const char *ma5w_last_error(void) { return ma5t_last_error(); }
EMSCRIPTEN_KEEPALIVE
int ma5w_compact_mode(void) { return g_compact_mode; }
EMSCRIPTEN_KEEPALIVE
int ma5w_loaded(void) { return g_t != NULL; }
