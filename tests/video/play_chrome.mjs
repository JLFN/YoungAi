/* play_chrome.mjs — 视频播放门: 系统 Chrome(Playwright channel=chrome)里用 <video> 把 mp4 从头播到 ended。
 * 为什么要它: vidcheck 证明系统解码器逐字节对, 这里证明浏览器(安卓微信 / 钉钉内置浏览器同一类内核)认这个容器、能连续播完、
 * 尺寸和时长报得对。播放开 16 倍速, 一个 2 秒的片子不到 1 秒。
 * 用法: node tests/video/play_chrome.mjs A.mp4 [B.mp4 …]   (找 playwright 的办法同 tests/asst/e2e_device.mjs)
 * 出错会怎样: 容器坏 → error 事件 code 4(MEDIA_ERR_SRC_NOT_SUPPORTED); 码流中途坏 → code 3 或卡住不到 ended(超时 30 s 判 FAIL)。 */
import { existsSync, readdirSync } from "node:fs";
import { homedir } from "node:os";
import path from "node:path";
import { pathToFileURL } from "node:url";

const files = process.argv.slice(2);
if (!files.length) { console.error("usage: node tests/video/play_chrome.mjs A.mp4 [B.mp4 …]"); process.exit(2); }
const HOME = homedir();
const pwDir = readdirSync(path.join(HOME, ".npm", "_npx")).map(d => path.join(HOME, ".npm", "_npx", d, "node_modules", "playwright")).find(existsSync);
if (!pwDir) { console.error("找不到 playwright 包; npx playwright 一次让它进缓存"); process.exit(2); }
const { chromium } = await import(path.join(pwDir, "index.mjs"));

const browser = await chromium.launch({ channel: "chrome", headless: true, args: ["--autoplay-policy=no-user-gesture-required", "--allow-file-access-from-files"] });
const page = await browser.newPage();
let fails = 0;
for (const f of files) {
    const url = pathToFileURL(path.resolve(f)).href;
    await page.goto(pathToFileURL(path.resolve(path.dirname(f))).href + "/");   /* 同目录的 file:// 源, 才读得到片子 */
    const r = await page.evaluate(async src => {
        const v = document.createElement("video");
        v.muted = true; v.playsInline = true; v.src = src; document.body.appendChild(v);
        const out = await new Promise(res => {
            const t = setTimeout(() => res({ err: "timeout at " + v.currentTime }), 30000);
            v.onerror = () => { clearTimeout(t); res({ err: "media error " + (v.error && v.error.code) }); };
            v.onloadedmetadata = () => { v.playbackRate = 16; v.play().catch(e => res({ err: "play(): " + e.message })); };
            v.onended = () => { clearTimeout(t); res({ ended: true }); };
        });
        const q = v.getVideoPlaybackQuality ? v.getVideoPlaybackQuality() : {};
        return { ...out, w: v.videoWidth, h: v.videoHeight, dur: v.duration, decoded: q.totalVideoFrames, dropped: q.droppedVideoFrames };
    }, url);
    const good = r.ended && !r.err && r.w > 0 && r.h > 0 && isFinite(r.dur);
    console.log(`${good ? "ok  " : "FAIL"} ${path.basename(f)}: ${r.w}x${r.h}, ${r.dur && r.dur.toFixed(3)} s, decoded ${r.decoded}, dropped ${r.dropped}${r.err ? ", " + r.err : ""}`);
    if (!good) fails++;
}
await browser.close();
console.log(fails ? `play_chrome: FAIL ${fails}/${files.length}` : `play_chrome: OK ${files.length} played to ended`);
process.exit(fails ? 1 : 0);
