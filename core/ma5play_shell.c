/* ma5play_shell.c — ma5t WASM 壳层
 * 编译进 libma5t_exp 五件套（i386/fpu/pe_loader/win32_stubs/ma5t_host），
 * 对 JS 导出扁平 C API；渲染循环语义与 test_render_t.c 一致：
 *   pump_seq → pump_audio → take_pcm（48kHz s16 立体声，192000 B/s）。
 * 数据模式：
 *   flat  — ma5w_init() 前不调 set_preload；快照经 Emscripten FS
 *           fopen("m5_snapshot.bin")（node 用 NODERAWFS，web 用 --preload-file）
 *   compact — 先 ma5w_set_preload(imgPtr, imgSize)（映像已在 HEAPU8），
 *           init 后走 ma5t_preload_boot_mem，flat 2GB 永不分配 */
#include <emscripten.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct ma5t_ctx ma5t_ctx_t;
extern int  ma5t_init(const char *dir, ma5t_ctx_t **out);
extern int  ma5t_load(ma5t_ctx_t *, const void *, uint32_t);
extern int  ma5t_open_standby_start(ma5t_ctx_t *);
extern void ma5t_pump_seq(ma5t_ctx_t *);
extern void ma5t_pump_audio(ma5t_ctx_t *);
extern uint32_t ma5t_take_pcm(ma5t_ctx_t *, uint8_t *, uint32_t);
extern const char *ma5t_last_error(void);
extern void ma5t_set_preload_image(const unsigned char *img, unsigned int size);
extern void ma5t_shutdown(ma5t_ctx_t *c);   /* 切曲：释放引擎后重新 init */
extern int g_compact_mode;

static ma5t_ctx_t *g_ctx = NULL;
static void *g_mmf = NULL;

EMSCRIPTEN_KEEPALIVE
void ma5w_set_preload(const unsigned char *img, unsigned int size) {
    ma5t_set_preload_image(img, size);
}

EMSCRIPTEN_KEEPALIVE
int ma5w_init(void) {
    g_ctx = NULL;
    return ma5t_init(NULL, &g_ctx);   /* dir=NULL → 内嵌 DLL 路径 */
}

EMSCRIPTEN_KEEPALIVE
int ma5w_load(const void *mmf, uint32_t size) {
    /* 一次 init 只支持一次 load（DLL 状态机不可重入）：切曲 = shutdown + 重新
     * init（compact 预载模式下 init 廉价，g_preload_img 静态保留有效） */
    if (g_ctx) { ma5t_shutdown(g_ctx); g_ctx = NULL; }
    if (ma5t_init(NULL, &g_ctx) != 0) return -1;
    free(g_mmf);
    g_mmf = malloc(size);             /* ma5t_load 可能持有指针，宿主侧保活 */
    if (!g_mmf) return -1;
    memcpy(g_mmf, mmf, size);
    return ma5t_load(g_ctx, g_mmf, size);
}

EMSCRIPTEN_KEEPALIVE
int ma5w_open_standby_start(void) {
    if (!g_ctx) return -1;
    return ma5t_open_standby_start(g_ctx);
}

EMSCRIPTEN_KEEPALIVE
void ma5w_pump_seq(void) { if (g_ctx) ma5t_pump_seq(g_ctx); }

EMSCRIPTEN_KEEPALIVE
void ma5w_pump_audio(void) { if (g_ctx) ma5t_pump_audio(g_ctx); }

EMSCRIPTEN_KEEPALIVE
uint32_t ma5w_take_pcm(uint8_t *dst, uint32_t max) {
    if (!g_ctx) return 0;
    return ma5t_take_pcm(g_ctx, dst, max);
}

EMSCRIPTEN_KEEPALIVE
const char *ma5w_last_error(void) { return ma5t_last_error(); }

EMSCRIPTEN_KEEPALIVE
int ma5w_compact_mode(void) { return g_compact_mode; }

EMSCRIPTEN_KEEPALIVE
int ma5w_loaded(void) { return g_ctx != NULL; }
