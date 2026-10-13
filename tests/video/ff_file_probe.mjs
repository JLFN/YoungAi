/* ff_file_probe.mjs — 探针(不进门): 火狐里 file:// 页面的子资源, BiDi 网络拦截看不看得到? CSP 能不能把本地文件限制在一个目录?
 * 为什么要问: make_video 渲染的是模型写的页面。页面要是能 <img src="file:///别处/照片.jpg">, 画面渲进视频就等于绕过了工作区边界。
 * 用法: node tests/video/ff_file_probe.mjs   (macOS: /Applications/Firefox.app; 打印每个拦到的请求和每张图读没读到) */
import { spawn } from "node:child_process";
import { mkdtempSync, mkdirSync, writeFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";

const FF = process.platform === "darwin" ? "/Applications/Firefox.app/Contents/MacOS/firefox" : "/snap/bin/firefox";
const root = mkdtempSync(path.join(tmpdir(), "ffprobe-"));
const ws = path.join(root, "ws"), out = path.join(root, "outside");
mkdirSync(ws); mkdirSync(out); mkdirSync(path.join(root, "prof"));
const png = Buffer.from("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==", "base64");
writeFileSync(path.join(ws, "in.png"), png); writeFileSync(path.join(out, "secret.png"), png); writeFileSync(path.join(out, "secret.txt"), "SECRET");
const wsUrl = "file://" + ws + "/";
const body = `<img id=a src="in.png"><img id=b src="../outside/secret.png"><img id=c src="https://example.com/x.png">
<iframe id=f src="../outside/secret.txt"></iframe>
<script>window.__r={};fetch('https://example.com/y').then(()=>__r.fetch='ok',e=>__r.fetch='blocked:'+e.message);
fetch('../outside/secret.txt').then(r=>r.text()).then(t=>__r.ffile='read:'+t,e=>__r.ffile='blocked:'+e.message);</script>`;
writeFileSync(path.join(ws, "plain.html"), `<!doctype html><html><body>${body}</body></html>`);
const csp = `default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src ${wsUrl} data: blob:; media-src ${wsUrl} data: blob:; font-src ${wsUrl} data:; connect-src 'none'; frame-src 'none'`;
writeFileSync(path.join(ws, "csp.html"), `<!doctype html><html><head><meta http-equiv="Content-Security-Policy" content="${csp}"></head><body>${body}</body></html>`);

const port = 9300 + Math.floor(Math.random() * 500);
const ff = spawn(FF, ["--headless", "--no-remote", "--profile", path.join(root, "prof"), "--remote-debugging-port", String(port)], { stdio: "ignore" });
const sleep = ms => new Promise(r => setTimeout(r, ms));
let sock;
for (let i = 0; i < 80 && !sock; i++) { await sleep(250); try { const s = new WebSocket(`ws://127.0.0.1:${port}/session`); await new Promise((res, rej) => { s.onopen = res; s.onerror = rej; }); sock = s; } catch { } }
if (!sock) { console.log("firefox did not start"); ff.kill(); process.exit(1); }
let id = 0; const wait = new Map(); const seen = [];
sock.onmessage = m => {
    const j = JSON.parse(m.data);
    if (j.id && wait.has(j.id)) { wait.get(j.id)(j); wait.delete(j.id); return; }
    if (j.method === "network.beforeRequestSent") {
        const u = j.params.request.url; seen.push(`${j.params.isBlocked ? "blocked" : "seen"} ${u}`);
        if (j.params.isBlocked) send(u.startsWith(wsUrl) ? "network.continueRequest" : "network.failRequest", { request: j.params.request.request });
    }
};
const send = (method, params) => new Promise(res => { const i = ++id; wait.set(i, res); sock.send(JSON.stringify({ id: i, method, params })); });
await send("session.new", { capabilities: {} });
await send("session.subscribe", { events: ["network.beforeRequestSent"] });
const tree = await send("browsingContext.getTree", {}); const ctx = tree.result.contexts[0].context;
await send("network.addIntercept", { phases: ["beforeRequestSent"], urlPatterns: [], contexts: [ctx] });
for (const page of ["plain.html", "csp.html"]) {
    seen.length = 0;
    await send("browsingContext.navigate", { context: ctx, url: wsUrl + page, wait: "complete" });
    await sleep(1500);
    const r = await send("script.evaluate", { expression: "JSON.stringify({a:document.getElementById('a').naturalWidth,b:document.getElementById('b').naturalWidth,c:document.getElementById('c').naturalWidth,frame:(()=>{try{return document.getElementById('f').contentDocument?.body?.innerText||'(no access)'}catch(e){return 'err:'+e.message}})(),...window.__r})", target: { context: ctx }, awaitPromise: false });
    console.log(`== ${page}\n  intercepted: ${seen.length ? seen.join("\n               ") : "(none)"}\n  result: ${r.result?.result?.value ?? JSON.stringify(r)}`);
}
sock.close(); ff.kill(); rmSync(root, { recursive: true, force: true });
