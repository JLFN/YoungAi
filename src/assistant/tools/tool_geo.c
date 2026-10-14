/* tool_geo.c — 地点数据源: 高德 Web 服务(坐标 → 地址; 周边 POI)。10-12 用户手机实测"附近有什么吃的"跑了 11 轮 30 次工具调用没出结果:
 * 手机只报了坐标(±9 m, 精度没问题), 页面反查地址走的 OSM 在国内手机网络连不上, 模型拿着一串数字用 Bing 猜它在哪条街 —— 通用搜索不做地理检索, 猜不出来。
 * 所以: ① 反查地址在服务端做, 设备行里直接给"××市××区××路"; ② 给模型一个 nearby 工具, 一次回 10 家真实 POI(名字/距离/地址/电话/评分/坐标)。
 * key: ~/.ds4/assistant/secrets/amap_key(0600, 方案稿 §4.5): C 在发请求那一刻填进 URL, 模型的上下文里永远没有它; 工具参数里只有 query。
 * 请求经火狐走 https(本项目不引 TLS 库; 火狐关了 JSON 查看器, 页面正文就是原始 JSON)。
 * ★反查地址不要 key★(10-12 用户: "位置没有免费获取的方式吗"): 高德网页版自己用的 www.amap.com/service/regeo 不带 key 就回省市区 + 最近几条路(spark 实测);
 *   它不是公开接口, 哪天改了只会让地址退回坐标, 不会坏别的。周边 POI 那条(service/poiInfo)页内 fetch 也回 419(阿里 WAF), 所以真 POI 仍要 key;
 *   没 key 的 nearby 工具不消失, 自己降级成"地址 + 关键词"的网页搜索(nearby_by_search) —— 工具永远在, 只是结果档次不同。
 * 没火狐 → 什么都不做, 行为退回只有坐标。
 * 出错会怎样: 高德回 status != "1"(key 错、额度用完) → 原文 info 给模型, 它会告诉你。 */
#include "../asst_internal.h"
#include <limits.h>   /* web_internal.h 用 PATH_MAX 却不自带 */
#include "../../web/web_internal.h"
#include <ctype.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define GEO_MAX_POI 10
#define GEO_RADIUS_DEFAULT 1500   /* 米: 步行 20 分钟的圈; 模型可按用户说的"500 米内"收窄 */
#define GEO_RADIUS_MAX 50000

static void secret_path(const char *name, char *p, size_t n) { snprintf(p, n, "%s/secrets/%s", asst_root, name); }

static bool name_ok(const char *name) {
    if (!name || !name[0] || strlen(name) > 32) return false;
    for (const char *c = name; *c; c++) if (!(islower((unsigned char)*c) || isdigit((unsigned char)*c) || *c == '_')) return false;
    return true;
}

bool asst_secret_get(const char *name, char *out, size_t n) {
    out[0] = 0;
    if (!name_ok(name)) return false;
    char p[ASST_PATH + 64]; secret_path(name, p, sizeof p);
    char *s = asst_slurp(p, NULL);
    if (!s) return false;
    s[strcspn(s, "\r\n")] = 0;
    snprintf(out, n, "%s", s); free(s);
    return out[0] != 0;
}

/* 值只许可见 ASCII(key/令牌都是); 空值 = 删掉 */
bool asst_secret_set(const char *name, const char *value, char *err, size_t errn) {
    if (!name_ok(name)) { snprintf(err, errn, "secret name must be [a-z0-9_]{1,32}"); return false; }
    char d[ASST_PATH + 16]; snprintf(d, sizeof d, "%s/secrets", asst_root); mkdir(d, 0700);
    char p[ASST_PATH + 64]; secret_path(name, p, sizeof p);
    if (!value || !value[0]) { unlink(p); return true; }
    if (strlen(value) > 256) { snprintf(err, errn, "value too long"); return false; }
    for (const char *c = value; *c; c++) if (*c <= ' ' || *c > '~') { snprintf(err, errn, "value must be printable ASCII without spaces"); return false; }
    if (!asst_write_file(p, value, strlen(value), 0600)) { snprintf(err, errn, "cannot write %s", p); return false; }
    return true;
}

bool asst_geo_ready(void) { return asst_browser_kind()[0] != 0; }   /* 反查地址只要火狐 */
bool asst_geo_nearby_ready(void) { char k[257]; return asst_secret_get("amap_key", k, sizeof k) && asst_browser_kind()[0]; }

/* 高德 JSON → 一句地址; 失败 NULL, err 给原因 */
char *asst_geo_parse_regeo(const char *json, char *err, size_t errn) {
    char *st = NULL, *info = NULL, *rg = NULL, *addr = NULL;
    asst_jstr(json, "status", &st);
    if (!st || strcmp(st, "1")) { asst_jstr(json, "info", &info); snprintf(err, errn, "amap: %s", info ? info : "bad response"); free(st); free(info); return NULL; }
    free(st);
    if (!asst_jraw(json, "regeocode", &rg) || !asst_jstr(rg, "formatted_address", &addr) || !addr[0]) { snprintf(err, errn, "amap: no address for this point"); free(rg); free(addr); return NULL; }
    free(rg);
    return addr;
}

/* 网页版 regeo 的 JSON: data.{province,city,district,road_list[{name,distance,direction}]} → "江苏省南京市浦口区 西江路附近(121 m)" */
char *asst_geo_parse_regeo_free(const char *json, char *err, size_t errn) {
    char *st = NULL, *d = NULL;
    asst_jstr(json, "status", &st);
    if (!st || strcmp(st, "1") || !asst_jraw(json, "data", &d) || d[0] != '{') { snprintf(err, errn, "amap web regeo: bad response"); free(st); free(d); return NULL; }
    free(st);
    ds4_buf b = {0};
    static const char *const keys[] = { "province", "city", "district", NULL };
    for (int i = 0; keys[i]; i++) { char *v = NULL; if (asst_jstr(d, keys[i], &v) && v[0] && !(b.len && strstr(b.ptr, v))) ds4_buf_puts(&b, v); free(v); }   /* 直辖市 province==city, 不重复 */
    char *roads = NULL, *r0 = NULL;
    if (asst_jraw(d, "road_list", &roads) && asst_jarr_at(roads, 0, &r0)) {
        char *name = NULL, *dist = NULL;
        if (asst_jstr(r0, "name", &name) && name[0]) { ds4_buf_printf(&b, " %s附近", name); if (asst_jstr(r0, "distance", &dist) && dist[0]) ds4_buf_printf(&b, "(%s m)", dist); }
        free(name); free(dist);
    }
    free(roads); free(r0); free(d);
    if (!b.len) { snprintf(err, errn, "amap web regeo: no address fields"); ds4_buf_free(&b); return NULL; }
    return ds4_buf_take(&b);
}

static void put_field(ds4_buf *b, const char *obj, const char *key, const char *label) {   /* 高德空字段给的是 [] 不是 "", asst_jstr 会拒, 正好跳过 */
    char *v = NULL;
    if (asst_jstr(obj, key, &v) && v[0]) { ds4_buf_puts(b, label); ds4_buf_puts(b, v); }
    free(v);
}

/* 高德周边 JSON → 给模型看的清单(一行一家) */
char *asst_geo_parse_around(const char *json, char *err, size_t errn) {
    char *st = NULL, *info = NULL, *pois = NULL, *p = NULL;
    asst_jstr(json, "status", &st);
    if (!st || strcmp(st, "1")) { asst_jstr(json, "info", &info); snprintf(err, errn, "amap: %s", info ? info : "bad response"); free(st); free(info); return NULL; }
    free(st);
    if (!asst_jraw(json, "pois", &pois)) { snprintf(err, errn, "amap: no pois field"); return NULL; }
    ds4_buf b = {0}; int n = 0;
    for (int i = 0; i < GEO_MAX_POI && asst_jarr_at(pois, i, &p); i++, free(p), p = NULL) {
        char *name = NULL; if (!asst_jstr(p, "name", &name) || !name[0]) { free(name); continue; }
        ds4_buf_printf(&b, "%d. %s", ++n, name); free(name);
        put_field(&b, p, "distance", " · "); { char *d = NULL; if (asst_jstr(p, "distance", &d) && d[0]) ds4_buf_puts(&b, " m"); free(d); }
        put_field(&b, p, "type", " · ");
        char *be = NULL;
        if (asst_jraw(p, "biz_ext", &be) && be[0] == '{') { put_field(&b, be, "rating", " · rating "); put_field(&b, be, "cost", " · avg cost "); }
        free(be);
        put_field(&b, p, "address", "\n   address: "); put_field(&b, p, "tel", "\n   tel: "); put_field(&b, p, "location", "\n   location(lng,lat): ");
        ds4_buf_putc(&b, '\n');
    }
    free(pois);
    if (!n) ds4_buf_puts(&b, "(no places found within this radius; try a larger radius or a different query)\n");
    return ds4_buf_take(&b);
}

static char *amap_get(const char *path_and_query, char *err, size_t errn) {
    char key[257];
    if (!asst_secret_get("amap_key", key, sizeof key)) { snprintf(err, errn, "no amap_key saved in settings"); return NULL; }
    ds4_buf u = {0}; ds4_buf_printf(&u, "https://restapi.amap.com/v3/%s&key=%s", path_and_query, key);
    char *txt = asst_web_text(u.ptr, err, errn);   /* 带 key 的 URL 只在这里存在, 不进事件、不进审计 */
    ds4_buf_free(&u);
    memset(key, 0, sizeof key);
    return txt;
}

/* 会话里记住的上次定位: meta.loc = {"lat":…,"lng":…,"acc":…,"addr":"…","t":…}。locate 成功一次就写, 之后每条消息的事实行都带着, 模型不用反复定位(10-12 用户定) */
bool asst_geo_loc_get(const char *sid, double *lat, double *lng, long *acc, char *addr, size_t n, long *t) {
    char *loc = NULL, *la = NULL, *lg = NULL, *ad = NULL; bool ok = false;
    if (sid && asst_meta_get(sid, "loc", &loc) && loc[0] == '{' && asst_jraw(loc, "lat", &la) && asst_jraw(loc, "lng", &lg)) {
        *lat = atof(la); *lng = atof(lg); *acc = 0; *t = 0; asst_jint(loc, "acc", acc); asst_jint(loc, "t", t);
        if (addr && n) { addr[0] = 0; if (asst_jstr(loc, "addr", &ad)) snprintf(addr, n, "%s", ad); }
        ok = true;
    }
    free(loc); free(la); free(lg); free(ad);
    return ok;
}

char *asst_geo_locate_post(const char *sid, const char *raw) {
    char *la = NULL, *lg = NULL; long acc = 0;
    if (!asst_jraw(raw, "lat", &la) || !asst_jraw(raw, "lng", &lg) || la[0] == '"' || lg[0] == '"') { free(la); free(lg); return NULL; }   /* 不是 {lat,lng}: 设备报的失败原因, 原样给模型 */
    const double lat = atof(la), lng = atof(lg); asst_jint(raw, "acc", &acc); free(la); free(lg);
    char err[256] = ""; char *addr = asst_geo_ready() ? asst_geo_regeo(lat, lng, err, sizeof err) : NULL;
    ds4_buf m = {0}; ds4_buf_printf(&m, "{\"lat\":%.6f,\"lng\":%.6f,\"acc\":%ld,\"t\":%ld,\"addr\":", lat, lng, acc, (long)time(NULL)); ds4_json_escape(&m, addr ? addr : ""); ds4_buf_putc(&m, '}');
    asst_meta_set(sid, "loc", m.ptr); ds4_buf_free(&m);
    ds4_buf r = {0};
    ds4_buf_printf(&r, "lat,lng: %.6f,%.6f", lat, lng);
    if (acc > 0) ds4_buf_printf(&r, " (±%ld m)", acc);
    if (addr) ds4_buf_printf(&r, "\naddress: %s", addr);
    else ds4_buf_printf(&r, "\naddress: unavailable (%s)", err[0] ? err : "no map service on this machine");
    ds4_buf_puts(&r, "\n(remembered for this conversation; later messages show it as the last fix)\n");
    free(addr);
    return ds4_buf_take(&r);
}

char *asst_geo_regeo(double lat, double lng, char *err, size_t errn) {
    char key[257];
    if (asst_secret_get("amap_key", key, sizeof key)) {   /* 有 key 走正式接口(门牌号级); 没 key 走网页版自用接口(省市区 + 最近的路) */
        memset(key, 0, sizeof key);
        char q[160]; snprintf(q, sizeof q, "geocode/regeo?location=%.6f,%.6f&extensions=base&output=JSON", lng, lat);
        char *j = amap_get(q, err, errn);
        if (!j) return NULL;
        char *a = asst_geo_parse_regeo(j, err, errn); free(j);
        return a;
    }
    char u[160]; snprintf(u, sizeof u, "https://www.amap.com/service/regeo?longitude=%.6f&latitude=%.6f", lng, lat);
    char *j = asst_web_text(u, err, errn);
    if (!j) return NULL;
    char *a = asst_geo_parse_regeo_free(j, err, errn); free(j);
    return a;
}

/* 没 key 的降级: 拿反查到的地址(省市区 + 路名, 去掉"附近(121 m)"这种距离尾巴)和要找的东西拼成一条搜索, 走 Bing。
 * 为什么放在工具里而不是提示词里: 模型只需要知道"附近就调 nearby", 数据源有没有、怎么退化是这里的事; 换数据源不用动提示词。 */
static char *nearby_by_search(const char *addr, const char *query, char *err, size_t errn) {
    if (!addr || !addr[0]) { snprintf(err, errn, "no map data source configured and this point has no address, so there is nothing to search by; ask the user which area they are in, or open the map with navigate"); return NULL; }
    ds4_buf q = {0};
    const char *cut = strstr(addr, "附近(");
    ds4_buf_append(&q, addr, cut ? (size_t)(cut - addr) : strlen(addr));
    ds4_buf_putc(&q, ' '); ds4_buf_puts(&q, query);
    char *r = asst_web_search(q.ptr, err, errn);
    if (!r) { ds4_buf_free(&q); return NULL; }
    ds4_buf b = {0};
    ds4_buf_printf(&b, "# places near %s — web search for \"%s\" (no map data source configured: names come from review/listing pages, distances are not verified; visit_page a result for details)\n", addr, q.ptr);
    ds4_buf_puts(&b, r); free(r); ds4_buf_free(&q);
    return ds4_buf_take(&b);
}

char *asst_geo_nearby(double lat, double lng, const char *addr, const char *query, long radius, char *err, size_t errn) {
    if (radius <= 0) radius = GEO_RADIUS_DEFAULT;
    if (radius > GEO_RADIUS_MAX) radius = GEO_RADIUS_MAX;
    if (!query || !query[0]) query = "美食";
    if (!asst_geo_nearby_ready()) return nearby_by_search(addr, query, err, errn);
    char *enc = web_url_encode(query);
    ds4_buf q = {0}; ds4_buf_printf(&q, "place/around?location=%.6f,%.6f&keywords=%s&radius=%ld&offset=%d&page=1&extensions=all&output=JSON", lng, lat, enc, radius, GEO_MAX_POI);
    free(enc);
    char *j = amap_get(q.ptr, err, errn); ds4_buf_free(&q);
    if (!j) return NULL;
    char *r = asst_geo_parse_around(j, err, errn); free(j);
    if (!r) return NULL;
    ds4_buf b = {0}; ds4_buf_printf(&b, "# places within %ld m of the user (%.5f,%.5f)\n%s", radius, lat, lng, r); free(r);
    return ds4_buf_take(&b);
}
