/* web/sw.js — 工作台的 service worker(PWA 安装 + 断网能打开外壳)。
 * 只管两样: ①页面本身(导航请求) 网络优先, 断网才用缓存的那份 —— 所以改了 studio.html 刷新就生效, 不会被旧缓存卡住;
 * ②图标/manifest 缓存优先(几乎不变)。/api /v1 /metrics /monitor 一律不碰: 数据必须是实时的, 缓存一份旧状态比报"连不上"更坑人,
 * 助理的事件流(/api/assistant/stream)更不能被拦。
 * 浏览器只在 https 或 localhost / 127.0.0.1 下注册 service worker: 手机用 http://局域网IP:8000 打开时注册会静默失败,
 * 页面照常能用, 只是装不成 PWA(要装就套一层 https, 见 docs/pwa.md)。
 * 改了这里的缓存清单要把 CACHE 的版本号 +1: 新 worker 激活时删掉旧版本的缓存。 */
const CACHE = "ds4-studio-v2";   /* v2: manifest 加了 share_target(分享目标), 装过的手机要换掉缓存里的旧 manifest */
const SHELL = ["/", "/manifest.webmanifest", "/logo.jpg", "/icon-192.png", "/icon-512.png", "/icon-maskable.png", "/apple-touch-icon.png"];
const STATIC = new Set(SHELL.slice(1));
const PAGES = new Set(["/", "/index.html", "/studio", "/train"]);   /* 同 src/model/model_api.c 里 serve_page 认的路径 */

self.addEventListener("install", e => {
  e.waitUntil(caches.open(CACHE).then(c => c.addAll(SHELL)).then(() => self.skipWaiting()));
});
self.addEventListener("activate", e => {
  e.waitUntil(caches.keys().then(ks => Promise.all(ks.filter(k => k !== CACHE).map(k => caches.delete(k)))).then(() => self.clients.claim()));
});
self.addEventListener("fetch", e => {
  const req = e.request, url = new URL(req.url);
  if (req.method !== "GET" || url.origin !== location.origin) return;
  /* 只接工作台页本身; /monitor 等别的页照常走网络(断网时回落成工作台页会让人以为点错了) */
  if (req.mode === "navigate" && PAGES.has(url.pathname)) {   /* 网络优先; 拿到新的顺手更新缓存, 断网回落到缓存的外壳(引擎卡片会显示"连不上") */
    e.respondWith(fetch(req).then(r => {
      if (r.ok) { const cp = r.clone(); caches.open(CACHE).then(c => c.put("/", cp)); }
      return r;
    }).catch(() => caches.match("/")));
    return;
  }
  if (STATIC.has(url.pathname)) e.respondWith(caches.match(url.pathname).then(hit => hit || fetch(req)));
});

/* 推送(10-14): 服务端(src/assistant/asst_push.c)按 RFC 8291 加密发来 {title, body, url}, 浏览器解开后到这里; 页面没开着也能收。
   点通知: 有开着的工作台窗口就聚焦它并把 url 递过去(页面自己切到那个对话), 没有就开一个。
   服务端在有页面正开着看时不推(改成页面里弹), 所以这里不再判"页面可见就不显示" —— Chrome 对"收到推送却不显示通知"有配额, 多了会替你弹一条"网站已在后台更新"。 */
self.addEventListener("push", e => {
  let d = {};
  try { d = e.data ? e.data.json() : {}; } catch (_) { d = { body: e.data ? e.data.text() : "" }; }
  e.waitUntil(self.registration.showNotification(d.title || "YoungAi", { body: d.body || "", icon: "/icon-192.png", badge: "/icon-192.png", data: { url: d.url || "/" } }));
});
self.addEventListener("notificationclick", e => {
  e.notification.close();
  const url = (e.notification.data && e.notification.data.url) || "/";
  e.waitUntil(self.clients.matchAll({ type: "window", includeUncontrolled: true }).then(cs => {
    const c = cs.find(x => x.url.startsWith(self.location.origin));
    if (c) { c.postMessage({ notice: url }); return c.focus(); }
    return self.clients.openWindow(url);
  }));
});
