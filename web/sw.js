/* web/sw.js — 工作台的 service worker(PWA 安装 + 断网能打开外壳)。
 * 只管两样: ①页面本身(导航请求) 网络优先, 断网才用缓存的那份 —— 所以改了 studio.html 刷新就生效, 不会被旧缓存卡住;
 * ②图标/manifest 缓存优先(几乎不变)。/api /v1 /metrics /monitor 一律不碰: 数据必须是实时的, 缓存一份旧状态比报"连不上"更坑人,
 * 助理的事件流(/api/assistant/stream)更不能被拦。
 * 浏览器只在 https 或 localhost / 127.0.0.1 下注册 service worker: 手机用 http://局域网IP:8000 打开时注册会静默失败,
 * 页面照常能用, 只是装不成 PWA(要装就套一层 https, 见 docs/pwa.md)。
 * 改了这里的缓存清单要把 CACHE 的版本号 +1: 新 worker 激活时删掉旧版本的缓存。 */
const CACHE = "ds4-studio-v1";
const SHELL = ["/", "/manifest.webmanifest", "/logo.jpg", "/icon-192.png", "/icon-512.png", "/icon-maskable.png", "/apple-touch-icon.png"];
const STATIC = new Set(SHELL.slice(1));
const PAGES = new Set(["/", "/index.html", "/studio", "/train"]);   /* 同 src/train/train_api.c 里 serve_page 认的路径 */

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
