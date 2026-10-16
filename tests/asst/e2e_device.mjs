/* e2e_device.mjs — 助理"进 → 干 → 出"的页面端到端门(总纲 docs/assistant_arch.md §10), make asst-e2e 跑。
 * 起真的 ds4-train(从 18000 起找空端口) + 假模型 tests/asst/fake/ds4-server(主进程端口 +1, 按对话里 tool 结果数走: 读剪贴板 → 写报告 → locate → run → 五个交出去的动作 → 终稿),
 * 用系统 Chrome(Playwright channel=chrome)仿真 iPhone(触屏 + 定位 + 剪贴板权限)跑一遍, 逐项核对:
 *   1 事实行: messages.jsonl 第一条 user 以 "[设备: 手机 · … · 可用: 定位/…/打开app]" 开头, 没有位置(位置是 locate 工具); 第二条消息带 "上次定位: …" 与 "[附件: …]"
 *   2 剪贴板 / 定位: 设备卡自动做, tool 结果 = 预放进剪贴板的文字 / "lat,lng: 30.274100,120.155100 (±35 m)"; 报告里有剪贴板内容; meta.loc 落盘
 *   3 run: 沙盒里跑、不审批(decision=auto), 生成的文件在工作区
 *   4 文件卡: share(path) 一张 + 终稿里点名、没 share 过的文件各一张(files 事件, 10-15): 下载得到的字节 == 盘上文件; 没有终稿卡; write_file 本身不出卡
 *   5 动作按钮: navigate / call / open_app 是真链接(iPhone UA 由页面自己定成 maps:// / tel: / snssdk1128://; 安卓 UA 的导航是 intent:geo + 高德网页兜底); copy_text 点后剪贴板 == 文本; share 带 text + 文件
 *   6 工具表协商: 第一次请求的 tools 里有 locate/read_clipboard/share/navigate/open_app/run; 电脑外壳 + 不安全上下文那次没有 clipboard/share/geolocation
 *   7 http 提示: 用局域网 IP 打开(不安全上下文)输入框上方出提示
 * 为什么不等页面进 serving 态: macOS 上主进程不扫进程表, 认不出外来的假模型; 这里直接调页面的 send()(发消息那条路照样走设备信息与能力表), serving 门是 train_model 的事, 另有单测。
 * 跑完: 杀两个进程; 测试建的会话挪进 ~/.ds4/assistant/trash/sessions/(与页面删除同路), 报告文件与 inbox 删掉; 本来没有登录文件的话把测试设的那份删掉。 */
import { spawn, execSync } from "node:child_process";
import { appendFileSync, existsSync, readFileSync, readdirSync, renameSync, rmSync, mkdirSync, statSync, writeFileSync } from "node:fs";
import { homedir } from "node:os";
import { createServer } from "node:net";
import path from "node:path";

/* 端口自己找空的(主进程用 PORT, 假模型用 PORT+1): 写死 18000 时本机正挂着 ssh -L 18000:…:8000 spark 隧道,
   ds4-train 绑 0.0.0.0:18000 照样成功(macOS 上 127.0.0.1 那个更具体的绑定优先), 测试就连到了 spark 上的真工作台, 报"用户名或密码不对", 看着像登录坏了。 */
const free = p => new Promise(res => { const s = createServer().once("error", () => res(false)).listen(p, "127.0.0.1", () => s.close(() => res(true))); });
let PORT = 18000; while (!(await free(PORT)) || !(await free(PORT + 1))) PORT += 10;
const ROOT = process.cwd(), HOME = homedir();
const ASST = path.join(HOME, ".ds4", "assistant"), AUTH = path.join(HOME, ".ds4", "studio", "auth");
/* 参数: [playwright 包路径] [--shots 目录](给了就在语音段关键时刻截图, 人眼看样子用; 门不看图) */
const argv = process.argv.slice(2), si = argv.indexOf("--shots"), SHOTS = si >= 0 ? argv.splice(si, 2)[1] : null;
const pwDir = argv[0] || readdirSync(path.join(HOME, ".npm", "_npx")).map(d => path.join(HOME, ".npm", "_npx", d, "node_modules", "playwright")).find(existsSync);
if (!pwDir) { console.error("找不到 playwright 包; 给路径当第一个参数, 或 npx playwright 一次让它进缓存"); process.exit(2); }
const { chromium, devices } = await import(path.join(pwDir, "index.mjs"));

let fails = 0; const ok = (c, m) => { console.log((c ? "ok   " : "FAIL ") + m); if (!c) fails++; };

/* 文案表不许重名键(10-14 真机实撞): JS 对象字面量里重名是后写的静默覆盖前面的 —— 云手机加的 ph_busy:"手机忙, 稍后再试" 盖掉了助理的
   ph_busy:"助理正在干活…", 助理一跑输入框和"按住 说话"长条就写"手机忙"。先把字符串内容剥空再数键, 值里的冒号不会误判 */
{
  const src = readFileSync(path.join(ROOT, "web/studio.html"), "utf8"), a = src.indexOf("const I18N = {"), b = src.indexOf("\n};", a);
  const body = src.slice(a, b).replace(/"(?:[^"\\\n]|\\.)*"/g, '""').replace(/`(?:[^`\\]|\\.)*`/g, "``");
  for (const [loc, blk] of [["zh", body.slice(body.indexOf("zh: {"), body.indexOf("\n  en: {"))], ["en", body.slice(body.indexOf("\n  en: {"))]]) {
    const seen = new Map(), dup = [];
    for (const m of blk.matchAll(/(?:^|[{,\s])([A-Za-z_]\w*)\s*:/gm)) { if (seen.has(m[1])) dup.push(m[1]); seen.set(m[1], true); }
    ok(seen.size > 300 && dup.length === 0, `文案表 ${loc}: ${seen.size} 个键, 重名 ${dup.length} 个${dup.length ? ": " + dup.join(",") : ""}`);
  }
}
const sleep = ms => new Promise(r => setTimeout(r, ms));
const hadAuth = existsSync(AUTH);
const before = new Set(existsSync(path.join(ASST, "sessions")) ? readdirSync(path.join(ASST, "sessions")) : []);
const fake = spawn("tests/asst/fake/ds4-server", [String(PORT + 1)], { stdio: ["ignore", "inherit", "inherit"] });
const train = spawn("./ds4-train", ["--port", String(PORT), "--host", "0.0.0.0", "--root", ROOT], { stdio: ["ignore", "ignore", "inherit"] });
const cleanup = () => { try { fake.kill(); } catch (_) {} try { train.kill(); } catch (_) {} };
process.on("exit", cleanup);
for (let i = 0; i < 50; i++) { try { if ((await fetch(`http://127.0.0.1:${PORT}/manifest.webmanifest`)).ok) break; } catch (_) {} await sleep(200); }

/* 假麦克风(语音段用): Chrome 把 tests/fixtures/asr/demo.wav 循环当麦克风输入, 不弹权限框 */
const browser = await chromium.launch({ channel: "chrome", headless: true, args: ["--use-fake-ui-for-media-stream", "--use-fake-device-for-media-stream", "--use-file-for-fake-audio-capture=" + path.join(ROOT, "tests/fixtures/asr/demo.wav")] });
async function openPhone(url, geo) {
  const ctx = await browser.newContext({ ...devices["iPhone 13"], geolocation: geo, permissions: ["geolocation", "clipboard-read", "clipboard-write"], locale: "zh-CN" });
  await ctx.addInitScript(() => {
    window.__shares = [];
    /* 照 Chromium share_service_impl.cc 的两张表拒(扩展名 + MIME 都要在表里, 否则 NotAllowedError "Permission denied")——
       原来的假 share 什么都收, 10-13 视频分享真机被拒(mp4 被贴成 application/octet-stream)这里看不见 */
    const EXT = /\.(avif|bmp|css|csv|ehtml|flac|gif|htm|html|ico|jfif|jpeg|jpg|m4a|m4v|mp3|mp4|mpeg|mpg|oga|ogg|ogm|ogv|opus|pdf|pja|pjpeg|png|shtm|shtml|svg|svgz|text|tif|tiff|txt|wav|weba|webm|webp|xbm)$/i;
    const MIME = new Set(["application/pdf", "audio/flac", "audio/mp3", "audio/mpeg", "audio/ogg", "audio/wav", "audio/webm", "audio/x-m4a", "image/avif", "image/bmp", "image/gif", "image/jpeg", "image/png", "image/svg+xml", "image/tiff", "image/webp", "image/x-icon", "image/x-ms-bmp", "image/x-xbitmap", "text/comma-separated-values", "text/css", "text/csv", "text/html", "text/plain", "text/x-csv", "video/mp4", "video/mpeg", "video/ogg", "video/webm"]);
    navigator.share = async d => {
      for (const f of d.files || []) if (!EXT.test(f.name) || !MIME.has(f.type)) throw new DOMException("Permission denied", "NotAllowedError");
      window.__shares.push({ text: d.text, title: d.title, files: (d.files || []).map(f => ({ name: f.name, type: f.type, size: f.size })) });
    };
    navigator.canShare = () => true;
  });
  const page = await ctx.newPage();
  page.on("pageerror", e => console.log("pageerror:", e.message));
  await page.goto(url);
  return { ctx, page };
}
async function login(page) {   /* 首次: 设置用户名密码(本机没有登录文件时); 有的话这台机器的测试账号得已存在 */
  const box = page.locator("#auth");
  await sleep(600);
  if (!(await box.isVisible())) return;
  await page.fill("#auth-user", "e2e_user"); await page.fill("#auth-pass", "E2e-pass-123");
  if (await page.locator("#auth-pass2-row").isVisible()) await page.fill("#auth-pass2", "E2e-pass-123");
  await page.click("#auth-go");
  /* 登录没过时把页面上的报错带出来: 光报"等 #auth 隐藏超时"看不出是密码规则、口令文件还是接口挂了 */
  await page.waitForSelector("#auth", { state: "hidden", timeout: 8000 }).catch(async e => { throw new Error("登录没过, 页面报错: " + JSON.stringify(await page.locator("#auth-err").textContent()) + "\n" + e.message); });
}

/* ---- 手机 + 安全上下文(127.0.0.1 算安全)全流程 ---- */
const { ctx, page } = await openPhone(`http://127.0.0.1:${PORT}/?shell=app`, { latitude: 30.2741, longitude: 120.1551, accuracy: 35 });
await login(page);
await page.evaluate(() => navigator.clipboard.writeText("hello 剪贴板"));
let sentCaps = null; page.on("request", r => { if (r.url().endsWith("/api/assistant/send")) sentCaps = JSON.parse(r.postData()); });
await page.evaluate(() => { inputEl.value = "附近有什么好吃的"; inputEl.disabled = false; return send(); });
await page.waitForFunction(() => view && view.items.some(i => i.kind === "end"), null, { timeout: 30000 }).catch(() => {});
const items = await page.evaluate(() => view.items.map(i => ({ kind: i.kind, name: i.name, done: i.done, ok: i.ok, decision: i.decision, args: i.args, reason: i.reason, content: i.content })));
ok(items.some(i => i.kind === "end" && i.reason === "done"), "运行跑完(done)");
ok(sentCaps && sentCaps.device.kind === "phone" && !sentCaps.device.loc && Array.isArray(sentCaps.files) && sentCaps.files.length === 0, "send 带设备信息, 不带位置(定位是工具): " + JSON.stringify(sentCaps && sentCaps.device));
ok(sentCaps && ["geolocation", "clipboard_read", "clipboard_write", "share", "navigate", "call", "file", "open_url", "open_app"].every(c => sentCaps.caps.includes(c)) && sentCaps.apps.join() === "douyin,wechat,dingtalk,alipay,meituan,eleme", "手机 + 安全上下文报全能力 + app 表: " + JSON.stringify(sentCaps && sentCaps.caps));
const sid = await page.evaluate(() => view.sid);
const readMsgs = () => readFileSync(path.join(ASST, "sessions", sid, "messages.jsonl"), "utf8").split("\n").filter(Boolean).map(l => JSON.parse(l));
let msgs = readMsgs();
const u0 = msgs.find(m => m.role === "user");
ok(u0 && /^\[设备: 手机 · \d{4}-\d\d-\d\d \d\d:\d\d \S+ · [\w\/+-]+ · 可用: 定位\/读剪贴板\/复制到剪贴板\/分享\/打开链接\/导航\/拨号\/选文件\/打开app\]\n附近有什么好吃的$/.test(u0.content), "事实行在用户文本前: 能力表, 没有位置: " + JSON.stringify(u0 && u0.content));
const clipRes = msgs.find(m => m.role === "tool" && m.tool_call_id === "call_clip");
ok(clipRes && clipRes.content === "hello 剪贴板", "剪贴板自动读回: " + JSON.stringify(clipRes && clipRes.content));
const locRes = msgs.find(m => m.role === "tool" && m.tool_call_id === "call_loc");
ok(locRes && locRes.content.startsWith("lat,lng: 30.274100,120.155100 (±35 m)\naddress: ") && locRes.content.includes("remembered for this conversation"), "locate 自动做, 结果经服务端收尾: " + JSON.stringify(locRes && locRes.content));
const meta = JSON.parse(readFileSync(path.join(ASST, "sessions", sid, "meta.json"), "utf8"));
ok(meta.loc && meta.loc.lat === 30.2741 && meta.loc.acc === 35 && meta.loc.t > 0, "meta.loc 落盘: " + JSON.stringify(meta.loc));
const runIt = items.find(i => i.kind === "tool" && i.name === "run");
ok(runIt && runIt.done && runIt.ok && runIt.decision === "auto", "run 在沙盒里跑, 不审批(auto): " + JSON.stringify(runIt && { done: runIt.done, ok: runIt.ok, decision: runIt.decision }));
const pdf = path.join(ASST, "workspace", "附近美食报告.pdf");
ok(existsSync(pdf) && readFileSync(pdf, "utf8") === "PDF-fake\n", "run 生成的文件在工作区");
const report = path.join(ASST, "workspace", "附近美食报告.md");
ok(existsSync(report) && readFileSync(report, "utf8").includes("hello 剪贴板"), "报告写进工作区且含剪贴板内容");
for (const n of ["navigate", "call", "copy_text", "share", "open_app"]) ok(items.some(i => i.kind === "tool" && i.name === n && i.done && i.ok), "动作工具 " + n + " 调了且立刻回");
/* 文件卡: share(path) 一张(.md) + 终稿里点名、没 share 过的一张(.pdf, files 事件); .md 终稿里也点名了但不重复出; write_file 本身不出卡; 没有终稿卡 */
await page.waitForSelector(".deliver", { timeout: 5000 }).catch(() => {});   /* 渲染有 90 ms 合并窗口, view 里有 end 时 DOM 可能还差一拍 */
await page.waitForFunction(() => document.querySelectorAll(".deliver").length >= 2, null, { timeout: 5000 }).catch(() => {});
const cards = { deliver: await page.locator(".deliver").count(), final: await page.locator("#chat .final").count(), names: await page.locator(".deliver .fname").allTextContents() };
ok(cards.deliver === 2 && cards.final === 0 && cards.names[0] === "附近美食报告.md" && cards.names[1] === "附近美食报告.pdf", "文件卡: share 的 .md + 终稿点名的 .pdf, 不重复: " + JSON.stringify(cards));
const filesEv = readFileSync(path.join(ASST, "sessions", sid, "events.jsonl"), "utf8").split("\n").filter(Boolean).map(l => JSON.parse(l)).find(e => e.ev === "files");
ok(filesEv && JSON.stringify(filesEv.paths) === JSON.stringify(["附近美食报告.md", "附近美食报告.pdf"]), "files 事件 = 终稿里点名的两个文件(页面再去掉 share 过的): " + JSON.stringify(filesEv));
const dl = page.waitForEvent("download", { timeout: 8000 });
await page.locator(".deliver button", { hasText: "下载" }).first().click();
const d = await dl; const dlPath = await d.path();
ok(dlPath && readFileSync(dlPath).equals(readFileSync(report)), "下载字节与盘上文件一致");
/* 预览(10-16 用户实撞: 点一次叠一张, 有时点了没反应): 取文件拖慢 800 ms 模拟穿透网络, 还没回来时连点两下 = 开了又关, 不许叠出两份;
   清掉缓存再点开 = 当下就有"读取中"占位、按钮实心(.on), 回来后占位换成内容、仍一份;
   条目被整张重建(流式时 dirty 就会)后预览还在、仍是一份; 再点关掉 = 零份、按钮不实心 */
await page.route("**/api/assistant/file?**", async r => { await sleep(800); await r.continue(); });
const pvBtn = () => page.locator(".deliver").first().locator("button.pvbtn"), pvN = () => page.locator(".deliver").first().locator(".pvbody").count();
await page.evaluate(() => { const b = document.querySelector(".deliver button.pvbtn"); b.click(); b.click(); }); await sleep(1200);
ok((await pvN()) === 0 && !(await pvBtn().getAttribute("class")).includes("on"), "预览连点两下 = 开了又关, 没叠出内容");
await page.evaluate(() => { for (const it of view.items) if (it.pv) { delete it.pv; it.dirty = true; } renderChat(); }); await sleep(200);
await pvBtn().click(); await sleep(100);
const pvLoad = await page.locator(".deliver").first().locator(".pvload").textContent().catch(() => null);
ok(pvLoad === "读取中…" && (await pvN()) === 1 && (await pvBtn().getAttribute("class")).includes("on"), "点开当下: 读取中占位 + 按钮实心: " + pvLoad);
await sleep(1200);
ok((await pvN()) === 1 && (await page.locator(".deliver").first().locator(".pvload").count()) === 0 && (await page.locator(".deliver").first().locator("pre.pvbody").count()) === 1, "内容回来: 占位换成正文, 仍一份");
await page.unroute("**/api/assistant/file?**");
await page.evaluate(() => { for (const it of view.items) it.dirty = true; renderChat(); }); await sleep(200);
ok((await pvN()) === 1 && (await pvBtn().getAttribute("class")).includes("on"), "卡重建后预览还在, 仍一份");
await pvBtn().click(); await sleep(200);
ok((await pvN()) === 0 && !(await pvBtn().getAttribute("class")).includes("on"), "再点关掉: 零份, 按钮恢复");
/* 动作按钮 */
const navHref = await page.locator(".tool.dev a", { hasText: "导航" }).getAttribute("href");
ok(navHref === "maps://?q=%E9%BE%99%E4%BA%95%E8%8D%89%E5%A0%82&sll=30.2741,120.1551", "导航是真链接, iPhone 走 maps:// 开地图 App: " + navHref);
/* 安卓(10-12 小米真机裸 geo: 没反应): 同一个 mapUrl 换安卓 UA, 必须是带高德网页兜底的 intent:; 测完删掉覆盖, 后面的检查还按 iPhone 走 */
const andHref = await page.evaluate(() => { Object.defineProperty(navigator, "userAgent", { configurable: true, get: () => "Mozilla/5.0 (Linux; Android 14; 23127PN0CC) Mobile" }); try { return mapUrl({ query: "龙井草堂" }); } finally { delete navigator.userAgent; } });
ok(andHref === "intent://0,0?q=%E9%BE%99%E4%BA%95%E8%8D%89%E5%A0%82#Intent;scheme=geo;action=android.intent.action.VIEW;S.browser_fallback_url=" + encodeURIComponent("https://uri.amap.com/search?keyword=%E9%BE%99%E4%BA%95%E8%8D%89%E5%A0%82&callnative=1") + ";end", "安卓导航走 intent:geo + 高德网页兜底: " + andHref);
ok(await page.locator(".tool.dev a", { hasText: "拨打 · +8657187979999" }).getAttribute("href") === "tel:+8657187979999", "拨打是 tel: 链接");
ok(await page.locator(".tool.dev a", { hasText: "打开 · 抖音" }).getAttribute("href") === "snssdk1128://search?keyword=%E9%BE%99%E4%BA%95%E8%8D%89%E5%A0%82", "open_app 走页面平台表, 抖音搜索 scheme");
/* 外卖两行(docs/assistant_deeplink.md): iPhone 裸 scheme 搜店名; 安卓包 intent: + H5 兜底(同 mapUrl 的做法) */
const mtHref = await page.evaluate(() => APPS.meituan.url({ query: "龙井草堂" }) + "\n" + APPS.eleme.url({ query: "龙井草堂" }));
ok(mtHref === "imeituan://www.meituan.com/search?q=%E9%BE%99%E4%BA%95%E8%8D%89%E5%A0%82\neleme://search?keyword=%E9%BE%99%E4%BA%95%E8%8D%89%E5%A0%82", "美团/饿了么 iPhone 走搜索 scheme: " + mtHref);
const mtAnd = await page.evaluate(() => { Object.defineProperty(navigator, "userAgent", { configurable: true, get: () => "Mozilla/5.0 (Linux; Android 14; 23127PN0CC) Mobile" }); try { return APPS.meituan.url({ query: "龙井草堂" }); } finally { delete navigator.userAgent; } });
ok(mtAnd === "intent://www.meituan.com/search?q=%E9%BE%99%E4%BA%95%E8%8D%89%E5%A0%82#Intent;scheme=imeituan;package=com.sankuai.meituan;S.browser_fallback_url=" + encodeURIComponent("https://h5.waimai.meituan.com/waimai/mindex/home") + ";end", "安卓美团走 intent: + H5 兜底: " + mtAnd);await page.locator(".tool.dev button", { hasText: "复制" }).click(); await sleep(200);
ok((await page.evaluate(() => navigator.clipboard.readText())) === "龙井草堂 步行 8 分钟", "复制按钮写进剪贴板");
await page.locator(".tool.dev button", { hasText: "分享" }).click(); await sleep(400);
const shares = await page.evaluate(() => window.__shares);
/* Chromium(有 userAgentData)分享白名单不收 .md: shareable() 改名 .txt + text/plain, 内容一字不改(字节数 = 盘上 md 的大小) */
ok(shares.length === 1 && shares[0].text === "附近美食: 龙井草堂" && shares[0].files[0].name === "附近美食报告.txt" && shares[0].files[0].type === "text/plain" && shares[0].files[0].size === readFileSync(report).length, "share 动作带文字 + 文件(Chromium 下 md 改名 txt, 字节不变): " + JSON.stringify(shares[0]));
/* 视频走同一条取文件 → shareable → share 的路(make_video 出的 mp4); 取文件那步贴的类型不对时假 share 按 Chromium 规则拒 */
const vid = path.join(ASST, "workspace", "e2e_share.mp4"); writeFileSync(vid, Buffer.from("0000ftypisom-fake-mp4"));
const vshare = await page.evaluate(async () => { try { await navigator.share({ files: [shareable(await fileBlob("e2e_share.mp4"))] }); return window.__shares.at(-1).files[0]; } catch (e) { return { error: e.name + ": " + e.message }; } });
rmSync(vid, { force: true });
ok(vshare && vshare.name === "e2e_share.mp4" && vshare.type === "video/mp4" && vshare.size === 21, "mp4 分享贴 video/mp4, 过 Chromium 白名单: " + JSON.stringify(vshare));
ok(await page.evaluate(() => Object.keys(FILE_TYPES).length && "avif bmp css csv ehtml flac gif htm html ico jfif jpeg jpg m4a m4v mp3 mp4 mpeg mpg oga ogg ogm ogv opus pdf pja pjpeg png shtm shtml svg svgz text tif tiff txt wav weba webm webp xbm".split(" ").every(e => CHROME_SHARE_EXT.test("a." + e) && FILE_TYPES[e])), "FILE_TYPES 覆盖 Chromium 分享扩展名表的每一项");
ok(await page.evaluate(() => { const ds = [...document.querySelectorAll("details.think")]; return ds.length > 0 && ds.every(d => !d.open && d.querySelector("summary").textContent === "思考过程"); }), "思考过程边想边展开, 正文出来后自动收起");
/* 第二条消息: 附件按钮上传 → 事实行带上次定位 + 附件行; 本机有火狐时上次定位后面多一段反查出的地址(没火狐只有坐标), 两种都认 */
await page.locator("#attach-in").setInputFiles({ name: "合同.docx", mimeType: "application/octet-stream", buffer: Buffer.from("DOCX-bytes") });
await page.waitForFunction(() => pendingFiles.length === 1, null, { timeout: 5000 }).catch(() => {});
ok(await page.evaluate(() => pendingFiles.join()) === "inbox/合同.docx" && await page.locator("#attach-list .chip").count() === 1, "附件传进 inbox/ 并显示成小标签");
await page.evaluate(() => { inputEl.value = "转成 PDF"; inputEl.disabled = false; return send(); });
await page.waitForFunction(() => view && view.items.filter(i => i.kind === "end").length >= 2, null, { timeout: 15000 }).catch(() => {});
msgs = readMsgs();
const u1 = msgs.filter(m => m.role === "user")[1];
ok(u1 && /^\[设备: 手机 · [^\]]* · 上次定位: 30\.27410,120\.15510 ±35 m(?: [^\]]*?)? \(0 分钟前\)\]\n\[附件: inbox\/合同\.docx \(1 KB\)\]\n转成 PDF$/.test(u1.content), "第二条消息的事实行带上次定位 + 附件行: " + JSON.stringify(u1 && u1.content));
ok(await page.evaluate(() => pendingFiles.length) === 0, "发送后附件清空");
await ctx.close();

/* ---- 电脑外壳 + 不安全上下文(局域网 IP): 能力表缩水, 输入框上方出提示 ---- */
let lan = ""; for (const ifc of ["en0", "en1", "en2"]) { if (lan) break; try { lan = execSync(`ipconfig getifaddr ${ifc}`).toString().trim(); } catch (_) {} }
if (lan) {
  const ctx2 = await browser.newContext({ ...devices["iPhone 13"], locale: "zh-CN" });
  const p2 = await ctx2.newPage(); await p2.goto(`http://${lan}:${PORT}/?shell=app`); await login(p2);
  ok(await p2.waitForSelector("#dev-note.on", { state: "visible", timeout: 5000 }).then(() => true, () => false), "http 打开: 输入框上方提示位置/剪贴板/分享不可用");
  const caps2 = await p2.evaluate(() => deviceCaps());
  ok(!caps2.includes("clipboard_read") && !caps2.includes("share") && !caps2.includes("geolocation") && caps2.includes("navigate"), "不安全上下文不报剪贴板/分享/定位: " + JSON.stringify(caps2));
  await ctx2.close();
} else console.log("skip  没有 en0 地址, 跳过 http 提示项");
/* ---- 分享目标: 别的 App 分享进来 → 文字落进输入框, 地址栏去掉查询串 ---- */
{
  const ctx3 = await browser.newContext({ ...devices["iPhone 13"], locale: "zh-CN" });
  const p3 = await ctx3.newPage();
  await p3.goto(`http://127.0.0.1:${PORT}/?shell=app&title=${encodeURIComponent("我的位置")}&text=${encodeURIComponent("我在这里：江苏省南京市浦口区文德路8号 https://surl.amap.com/abc")}&url=${encodeURIComponent("https://surl.amap.com/abc")}`);
  await login(p3); await sleep(300);
  const v = await p3.evaluate(() => ({ input: inputEl.value, url: location.href, tab: curTab() }));
  ok(v.input === "我的位置\n我在这里：江苏省南京市浦口区文德路8号 https://surl.amap.com/abc\n" && v.tab === "chat" && !v.url.includes("text="), "分享进来落进输入框、切到助理页、查询串已去掉: " + JSON.stringify(v));
  await ctx3.close();
}
/* ---- 技能页(电脑外壳): 导航第三格是"技能"; 新建 → 盘上多了 skills/<名>/SKILL.md; 列表行带描述; 接口回 frontmatter 描述; 删除 → 挪进 trash/skills ---- */
const skDir = path.join(ASST, "skills", "e2e-weekly-report");
{
  rmSync(skDir, { recursive: true, force: true });
  const ctx4 = await browser.newContext({ locale: "zh-CN" });
  const p4 = await ctx4.newPage(); p4.on("pageerror", e => console.log("pageerror:", e.message));
  await p4.goto(`http://127.0.0.1:${PORT}/#skills`); await login(p4); await sleep(300);
  const navNames = await p4.evaluate(() => [...document.querySelectorAll("nav a[data-tab]")].filter(a => !a.hidden).map(a => a.dataset.tab));
  ok(navNames[1] === "tasks" && navNames[3] === "skills", "侧栏导航: 助理后面是任务(10-14), 技能在第四格(云手机没起时): " + navNames.join(","));
  /* 这台机器上可能本来就装着技能(10-14 起 Mac 上装着 pptx), 所以不假定空表: 只看页打开了、测试用的那个还不在 */
  ok(await p4.evaluate(() => curTab()) === "skills" && await p4.locator("#sk-list .row b", { hasText: "e2e-weekly-report" }).count() === 0, "技能页打开, 测试技能还不在");
  await p4.locator("#sk-new").click(); await p4.locator("#sk-name").fill("E2E Weekly Report");   /* 大写空格: 页面净化成小写连字符 */
  ok((await p4.locator("#sk-md").inputValue()).startsWith("---\nname: e2e-weekly-report\ndescription:"), "模板随名字净化后的名字填好");
  await p4.locator("#sk-md").fill("---\nname: e2e-weekly-report\ndescription: 写部门周报用这个\n---\n# 周报\n\n1. 读 reference/tpl.md\n");
  await p4.locator("#sk-save").click(); await sleep(300);
  ok(existsSync(path.join(skDir, "SKILL.md")) && readFileSync(path.join(skDir, "SKILL.md"), "utf8").includes("写部门周报用这个"), "保存 → skills/e2e-weekly-report/SKILL.md 落盘");
  ok(await p4.locator("#sk-list .row.sel b", { hasText: "e2e-weekly-report" }).count() === 1 && await p4.locator("#sk-list .row small", { hasText: "写部门周报用这个" }).count() === 1, "列表行: 名字 + 描述, 新建的被选中");
  /* 工具表里的 use_skill 行(模型那头)由 ds4_asst_test 的 test_skills 验; 这里只验页面 ↔ 接口 ↔ 盘 */
  const tj = await p4.evaluate(async () => (await aFetch("skills")).skills.map(s => s.name + ": " + s.description).join("|"));
  ok(tj.split("|").includes("e2e-weekly-report: 写部门周报用这个"), "GET skills 回名字与 frontmatter 描述: " + tj);
  await p4.locator("#sk-del").click(); await p4.locator("#ask-yes").click(); await sleep(300);
  ok(!existsSync(skDir) && readdirSync(path.join(ASST, "trash", "skills")).some(d => d.startsWith("e2e-weekly-report-")), "删除 → 文件夹挪进 trash/skills/");
  ok(await p4.locator("#sk-list .row b", { hasText: "e2e-weekly-report" }).count() === 0 && await p4.locator("#sk-edit").isHidden(), "删完列表里没了, 编辑卡收起");
  await ctx4.close();
}

/* ---- 记忆页(10-15 三类): 四个框按 data-mem 载入盘上四个文件; 改"怎么做事"点保存 → habits.md 变了, 别的文件不动; 测完原样写回(这是本机真的记忆目录) ---- */
{
  const memDir = path.join(ASST, "memory"), rd = f => existsSync(path.join(memDir, f)) ? readFileSync(path.join(memDir, f), "utf8") : null;
  const orig = Object.fromEntries(["profile.md", "habits.md", "env.md", "rules.md"].map(f => [f, rd(f)]));
  const ctxm = await browser.newContext({ locale: "zh-CN" });
  const pm = await ctxm.newPage(); pm.on("pageerror", e => console.log("pageerror:", e.message));
  await pm.goto(`http://127.0.0.1:${PORT}/#mem`); await login(pm); await sleep(300);
  await pm.evaluate(() => loadMem(true)); await sleep(300);
  const boxes = await pm.evaluate(() => [...document.querySelectorAll("#v-mem [data-mem]")].map(c => ({ key: c.dataset.mem, title: c.querySelector(".card-title").textContent, val: c.querySelector("textarea").value, path: c.querySelector("span.muted").textContent })));
  ok(boxes.map(b => b.key).join(",") === "profile,habits,env,rules" && boxes.map(b => b.title).join(",") === "关于你,怎么做事,设备与账号,规矩", "记忆页四个框: " + boxes.map(b => b.title).join(","));
  ok(boxes.every(b => b.val === (orig[b.path.split("/").pop()] || "") && b.path.endsWith("/memory/" + { profile: "profile.md", habits: "habits.md", env: "env.md", rules: "rules.md" }[b.key])), "每个框 = 盘上对应文件的原文");
  const mark = "- [e2e] 以后回答先给结论\n";
  await pm.locator('#v-mem [data-mem="habits"] textarea').fill((orig["habits.md"] || "") + mark);
  await pm.locator('#v-mem [data-mem="habits"] button').click(); await sleep(400);
  ok(rd("habits.md") === (orig["habits.md"] || "") + mark && rd("profile.md") === orig["profile.md"] && rd("env.md") === orig["env.md"] && rd("rules.md") === orig["rules.md"], "保存'怎么做事'只改 habits.md");
  for (const [f, v] of Object.entries(orig)) { const p = path.join(memDir, f); if (v === null) rmSync(p, { force: true }); else writeFileSync(p, v); }
  await ctxm.close();
}

/* ---- 任务页(10-14): 一句话创建 → 盘上 tasks/<id>.json + 专属会话(meta.task) → 假模型跑完第一遍 → 行里"跟进 1 次 · 下次 …" → 打开 = 进它的对话 → 删除进 trash
   10-16 页面上的一句话输入框和定时提醒卡删了(新建 = 右下角"+"进助理聊): 创建这一步改走同一个接口 POST tasks {op:add}, 后面照旧 ---- */
const tasksDir = path.join(ASST, "tasks");
{
  const beforeTasks = new Set(existsSync(tasksDir) ? readdirSync(tasksDir) : []);
  const ctx5 = await browser.newContext({ locale: "zh-CN" });
  const p5 = await ctx5.newPage(); p5.on("pageerror", e => console.log("pageerror:", e.message));
  await p5.goto(`http://127.0.0.1:${PORT}/#tasks`); await login(p5); await sleep(300);
  ok(await p5.evaluate(() => curTab()) === "tasks" && await p5.locator("#tk-fab").isVisible(), "任务页打开, 右下角有 + ");
  /* 通知卡(10-15)、一句话输入框与定时提醒卡(10-16)按用户令删了: 守住别回来 */
  await sleep(600);
  ok(await p5.locator("#nt-on, #nt-test, #nt-proxy, #tk-text, #rm-list").count() === 0, "任务页没有通知卡/输入框/提醒卡");
  await p5.locator("#tk-fab").click(); await sleep(300);
  ok(await p5.evaluate(() => curTab() + ":" + curSid) === "chat:null", "+ = 进助理开新对话");
  await p5.goto(`http://127.0.0.1:${PORT}/#tasks`); await sleep(400);
  await p5.evaluate(() => aFetch("tasks", { op: "add", text: "盯着 e2e 的价格, 降了通知我" }).then(r => { tasks = r.tasks || []; renderTasks(); })); await sleep(500);
  const newTask = readdirSync(tasksDir).filter(f => !beforeTasks.has(f));
  ok(newTask.length === 1, "回车创建 → tasks/ 多一个文件: " + newTask.join(","));
  const tk = newTask.length ? JSON.parse(readFileSync(path.join(tasksDir, newTask[0]), "utf8")) : {};
  ok(tk.state === "active" && tk.text === "盯着 e2e 的价格, 降了通知我" && /^[0-9a-f]{12}$/.test(tk.session || ""), "任务文件: active + 原句 + 会话 id");
  const meta = tk.session ? JSON.parse(readFileSync(path.join(ASST, "sessions", tk.session, "meta.json"), "utf8")) : {};
  ok(meta.task === newTask[0].replace(".json", ""), "会话 meta.task 指回任务");
  ok(await p5.locator("#tk-list .row b", { hasText: "盯着 e2e 的价格" }).count() === 1 && (await p5.locator("#nav-tasks").textContent()) === "1", "列表行出现, 侧栏任务数 1");
  let done = false;   /* 假模型(工具表里没有设备工具)直接出终稿: 几秒内 runs=1 */
  for (let i = 0; i < 40 && !done; i++) { await sleep(250); try { done = JSON.parse(readFileSync(path.join(tasksDir, newTask[0]), "utf8")).runs === 1; } catch (_) {} }
  const tk2 = JSON.parse(readFileSync(path.join(tasksDir, newTask[0]), "utf8"));
  ok(done && tk2.end === "done" && tk2.last.includes("龙井草堂") && tk2.next > Date.now() / 1000 + 3000, "第一遍跑完: runs=1, 上次进展=终稿头部, 没调 check_in 就排到一小时后");
  const msgs = readFileSync(path.join(ASST, "sessions", tk.session, "messages.jsonl"), "utf8").split("\n").filter(Boolean).map(l => JSON.parse(l));
  ok(msgs[0].role === "user" && /\[任务: 第 1 次跟进 · 创建于 /.test(msgs[0].content) && msgs[0].content.endsWith("盯着 e2e 的价格, 降了通知我"), "第一条 user 带任务事实行, 正文是原句");
  /* 通知(10-14 用户: "在 app 里什么都不弹, 离开后再推"): 这台浏览器没订阅 → notify 如实说没设备(不再假称"页面上显示了"), 页面里也不弹 */
  const ntf = msgs.find(m => m.role === "tool" && typeof m.content === "string" && m.content.includes("no device has notifications turned on"));
  ok(!!ntf, "notify 工具结果: 没设备开通知 → 直说没送到");
  await sleep(1500);
  ok(!(await p5.locator("#toast").textContent()).includes("e2e 通知"), "页面里没弹这条通知");
  await p5.locator("#tk-list .row b", { hasText: "盯着 e2e 的价格" }).waitFor(); await sleep(2800);   /* 等一轮 poll 把 runs/next 刷进行里 */
  const line = await p5.locator("#tk-list .row small").first().textContent();
  ok(/跟进 1 次/.test(line) && /下次 /.test(line), "行里: 跟进 1 次 · 下次 …: " + line);
  if (SHOTS) {   /* 任务卡长相(10-16 改成一任务一卡 + 右下角 +): 桌面一张, 手机一张 */
    mkdirSync(SHOTS, { recursive: true }); await p5.screenshot({ path: path.join(SHOTS, "tasks-desk.png") });
    const ph = await openPhone(`http://127.0.0.1:${PORT}/?shell=app#tasks`); await login(ph.page); await sleep(800);
    await ph.page.screenshot({ path: path.join(SHOTS, "tasks-phone.png") }); await ph.ctx.close();
  }
  await p5.locator("#tk-list .row button", { hasText: "暂停" }).first().click(); await sleep(400);
  ok(await p5.evaluate(() => curTab()) === "tasks" && JSON.parse(readFileSync(path.join(tasksDir, newTask[0]), "utf8")).state === "paused", "点卡上的按钮(暂停)不会顺带进对话");
  await p5.locator("#tk-list .row button", { hasText: "继续" }).first().click(); await sleep(400);
  await p5.locator("#tk-list .row b", { hasText: "盯着 e2e 的价格" }).click(); await sleep(400);
  ok(await p5.evaluate(() => curTab() + ":" + curSid) === "chat:" + tk.session, "点卡 = 进它的对话");
  await p5.goto(`http://127.0.0.1:${PORT}/#tasks`); await sleep(400);
  await p5.locator("#tk-list .row button", { hasText: "删除" }).first().click(); await p5.locator("#ask-yes").click(); await sleep(400);
  ok(!existsSync(path.join(tasksDir, newTask[0])) && readdirSync(path.join(ASST, "trash", "tasks")).some(f => f.startsWith(newTask[0].replace(".json", "") + "-")) && !existsSync(path.join(ASST, "sessions", tk.session)), "删除 → 任务进 trash/tasks, 会话进 trash/sessions");
  ok(await p5.locator("#tk-list .row b", { hasText: "盯着 e2e 的价格" }).count() === 0, "删完列表里没了");
  await ctx5.close();
}

/* ---- 计划(10-15, docs/assistant_plan.md): 一句话 → 假模型调 plan_propose → 计划卡(3 步 1 问, 开始/放弃) → 盘上 proposed 任务 → 开始 → 用户气泡"开始按计划做。" →
   第 1 步 ☑ 带备注 → 任务页 1/3; 另提一个点放弃 → 卡上"已放弃"、盘上 end=abandoned。进不进计划是模型(这里是假模型)调不调工具, 页面不猜 ---- */
{
  const beforeTasks = new Set(existsSync(tasksDir) ? readdirSync(tasksDir) : []);
  const ctx7 = await browser.newContext({ locale: "zh-CN" });
  const p7 = await ctx7.newPage(); p7.on("pageerror", e => console.log("pageerror:", e.message));
  await p7.goto(`http://127.0.0.1:${PORT}/#chat`); await login(p7); await sleep(300);
  const planTask = () => { for (const f of readdirSync(tasksDir)) if (!beforeTasks.has(f)) return { f, x: JSON.parse(readFileSync(path.join(tasksDir, f), "utf8")) }; return null; };
  const waitFor = async (fn, n = 60) => { for (let i = 0; i < n; i++) { try { const v = await fn(); if (v) return v; } catch (_) {} await sleep(250); } return null; };
  await p7.evaluate(() => { selectSession(null); inputEl.value = "e2e 计划: 周末去苏州玩"; inputEl.disabled = false; return send(); });
  const card = p7.locator(".plan", { hasText: "e2e 计划: 周末苏州" });
  await waitFor(async () => await card.count() === 1 && /你按开始我就动手/.test(await p7.locator("#chat").textContent()));
  const pshot = async n => { if (SHOTS) { mkdirSync(SHOTS, { recursive: true }); await sleep(300); await p7.screenshot({ path: path.join(SHOTS, n + ".png") }); } };
  ok(await card.count() === 1 && await card.locator(".ps").count() === 3 && await card.locator(".pq li").count() === 1, "计划卡: 3 步 1 问"); await pshot("plan-1-proposed");
  ok(await card.locator("button", { hasText: "开始" }).count() === 1 && await card.locator("button", { hasText: "放弃" }).count() === 1 && /等你确认/.test(await card.locator(".pst").textContent()), "待确认: 开始 / 放弃 两个按钮, 状态'等你确认'");
  const pt = planTask();
  ok(pt && pt.x.state === "proposed" && pt.x.steps.length === 3 && pt.x.steps[1].when === "2099-01-01 09:00", "盘上: proposed 任务, 步骤原文");
  const psid = pt ? pt.x.session : "";
  ok(psid && JSON.parse(readFileSync(path.join(ASST, "sessions", psid, "meta.json"), "utf8")).task === pt.f.replace(".json", ""), "这段对话 meta.task 指回计划");
  await card.locator("button", { hasText: "开始" }).click();
  const stepped = await waitFor(() => { const x = JSON.parse(readFileSync(path.join(tasksDir, pt.f), "utf8")); return x.steps[0].state === "done" && x.runs >= 2 && x; });
  ok(stepped && stepped.state === "active" && stepped.steps[0].note === "周六 8 点高铁" && stepped.next > Date.now() / 1000 + 1700 && stepped.next < Date.now() / 1000 + 1900, "开始后: active, 第 1 步 done 带备注, 下次 = check_in 的 30 分钟");
  await sleep(600);
  ok(await p7.locator(".msg.user", { hasText: "开始按计划做。" }).count() === 1, "对话里多一条用户气泡'开始按计划做。'");
  ok(await card.locator(".ps.done", { hasText: "定出发时间" }).count() === 1 && /进行中 1\/3/.test(await card.locator(".pst").textContent()) && await card.locator("button", { hasText: "开始" }).count() === 0, "卡上: 第 1 步 ☑, 进行中 1/3, 按钮收起"); await pshot("plan-2-started");
  await p7.goto(`http://127.0.0.1:${PORT}/#tasks`); await sleep(800);
  const row = p7.locator("#tk-list .row", { hasText: "e2e 计划: 周末苏州" });
  ok(await row.count() === 1 && /☑☐☐ 1\/3 · 下一步: 订酒店/.test(await row.textContent()), "任务页这行: ☑☐☐ 1/3 · 下一步: 订酒店");
  /* 放弃 */
  for (const f of readdirSync(tasksDir)) beforeTasks.add(f);
  await p7.goto(`http://127.0.0.1:${PORT}/#chat`); await sleep(300);
  await p7.evaluate(() => { selectSession(null); inputEl.value = "e2e 计划: 再来一个"; inputEl.disabled = false; return send(); });
  const card2 = p7.locator(".plan.ask", { hasText: "e2e 计划: 周末苏州" });
  await waitFor(async () => await card2.count() === 1 && /你按开始我就动手/.test(await p7.locator("#chat").textContent()));
  await card2.locator("button", { hasText: "放弃" }).click(); await p7.locator("#ask-yes").click();
  const pt2 = await waitFor(() => { const p = planTask(); return p && p.x.state === "done" && p; });
  ok(pt2 && pt2.x.end === "abandoned" && !pt2.x.done_at, "放弃 → 盘上 done + end=abandoned, 没有 done_at(不推'任务完成')");
  await waitFor(async () => /已放弃/.test(await p7.locator(".plan.closed").last().textContent()));
  ok(/已放弃/.test(await p7.locator(".plan.closed").last().textContent()) && await p7.locator(".plan.closed button").count() === 0, "卡上: 已放弃, 按钮收起");
  for (const f of readdirSync(tasksDir)) { try { if (JSON.parse(readFileSync(path.join(tasksDir, f), "utf8")).text.startsWith("e2e 计划")) rmSync(path.join(tasksDir, f), { force: true }); } catch (_) {} }   /* 测试建的计划任务文件; 会话由收尾挪进 trash */
  await ctx7.close();
}

/* ---- 语音输入(10-14, 用户: 照微信): 最右圆钮 麦克风 ⇄ 键盘 / 有字变发送; 按住说话 → 上滑取消 / 太短丢 / 松手识别完直接发 ----
   macOS 上主进程认不出假模型(tr_model_live 假): 页面不进 serving、/v1/ 也不转发。这里两处拦截, 页面代码不碰:
   /api/train/status 的 mode 改成 serving; /v1/audio/transcriptions 直接打到假模型端口(主进程转发这一段 10-14 在 spark 生产上实测过: 经 8000 转发 47 ms 同文本) */
{
  const ctx6 = await browser.newContext({ ...devices["iPhone 13"], permissions: ["microphone"], locale: "zh-CN" });
  await ctx6.route("**/api/train/status", async r => { const res = await r.fetch(); const j = await res.json(); j.mode = "serving"; await r.fulfill({ response: res, json: j }); });
  await ctx6.route("**/v1/audio/transcriptions", async r => r.fulfill({ response: await r.fetch({ url: `http://127.0.0.1:${PORT + 1}/v1/audio/transcriptions` }) }));
  const p6 = await ctx6.newPage(); p6.on("pageerror", e => console.log("pageerror:", e.message));
  const shot = async n => { if (SHOTS) { mkdirSync(SHOTS, { recursive: true }); await sleep(300); await p6.screenshot({ path: path.join(SHOTS, n + ".png") }); } };   /* 等一帧画完: 刚切新对话就截是空白 */
  const asrPosts = []; p6.on("request", r => { if (r.method() === "POST" && r.url().endsWith("/v1/audio/transcriptions")) asrPosts.push(r.url()); });
  await p6.goto(`http://127.0.0.1:${PORT}/?shell=app#chat`); await login(p6);
  await p6.evaluate(() => { lsSet("ds4.asst.voice", ""); voiceOn = false; selectSession(null); });
  await p6.waitForFunction(() => rightShown === "mic", null, { timeout: 8000 }).catch(() => {});
  const right = () => p6.evaluate(() => rightShown + "|" + sendBtn.className + "|" + document.querySelector(".composer").classList.contains("voice"));
  ok(await right() === "mic|ghost|false", "空输入 + 装了识别: 最右是麦克风: " + await right()); await shot("1-text-mic");
  await p6.locator("#input").fill("你好"); ok((await right()).startsWith("send|primary"), "打了字变发送: " + await right()); await shot("2-text-send");
  await p6.locator("#input").fill(""); ok((await right()).startsWith("mic|"), "删光变回麦克风");
  await p6.locator("#btn-send").click(); await sleep(400);
  ok(await right() === "kb|ghost|true" && await p6.locator("#btn-hold").isVisible() && await p6.locator("#input").isHidden() && (await p6.locator("#btn-hold").textContent()) === "按住 说话", "点麦克风: 输入框换成'按住 说话', 最右变键盘: " + await right()); await shot("3-voice-hold");
  const hb = await p6.locator("#btn-hold").boundingBox(), cx = hb.x + hb.width / 2, cy = hb.y + hb.height / 2;
  /* 上滑取消: 按住 → 上移 120 px → 浮层变红"松开手指, 取消发送" → 松手, 不上传 */
  await p6.mouse.move(cx, cy); await p6.mouse.down(); await sleep(900);
  ok(await p6.locator("#mic-ov").isVisible() && (await p6.locator("#mic-ov-t").textContent()) === "松开发送, 上滑取消" && (await p6.locator("#btn-hold").textContent()) === "松开 发送", "按住: 浮层'松开发送, 上滑取消', 长条'松开 发送'"); await shot("4-recording");
  await p6.mouse.move(cx, cy - 120, { steps: 4 }); await sleep(150);
  ok(await p6.evaluate(() => document.getElementById("mic-ov").classList.contains("cancel")) && (await p6.locator("#mic-ov-t").textContent()) === "松开手指, 取消发送" && (await p6.locator("#btn-hold").textContent()) === "松开 取消", "上滑: 浮层变红'松开手指, 取消发送'"); await shot("5-cancel-zone");
  await p6.mouse.up(); await sleep(600);
  ok(asrPosts.length === 0 && await p6.locator("#mic-ov").isHidden(), "上滑松手 = 取消: 没上传, 浮层收起");
  /* 太短: 0.2 s 就松 → "说话时间太短", 不上传 */
  await p6.mouse.move(cx, cy); await p6.mouse.down(); await sleep(200); await p6.mouse.up(); await sleep(500);
  ok(asrPosts.length === 0 && (await p6.locator("#toast").textContent()).includes("说话时间太短"), "按 0.2 s: 提示说话时间太短, 不上传");
  /* 正常: 按 2 s 松手 → 上传 WAV(假模型核头, 回时长) → 直接发出去, 输入框草稿不动 */
  await p6.evaluate(() => { inputEl.value = "草稿别动"; });
  await p6.mouse.move(cx, cy); await p6.mouse.down(); await sleep(2000); await p6.mouse.up();
  await p6.waitForFunction(() => view && view.items.some(i => i.kind === "user"), null, { timeout: 20000 }).catch(() => {}); await sleep(800);
  const sid6 = await p6.evaluate(() => view && view.sid);
  const u6 = sid6 ? readFileSync(path.join(ASST, "sessions", sid6, "messages.jsonl"), "utf8").split("\n").filter(Boolean).map(l => JSON.parse(l)).find(m => m.role === "user") : null;
  const ms = u6 && /假识别: 收到 (\d+) 毫秒音频$/.exec(u6.content);
  ok(asrPosts.length === 1 && ms && +ms[1] >= 1500 && +ms[1] <= 2600, "按 2 s 松手: 上传一次 16 kHz 单声道 WAV, 识别结果直接作为消息发出: " + JSON.stringify(u6 && u6.content.split("\n").pop()));
  ok(await p6.evaluate(() => inputEl.value) === "草稿别动", "语音发送不碰输入框里的草稿");
  /* 假模型这一轮会走设备工具(读剪贴板), 本上下文没给剪贴板权限 ⇒ 停在"等设备", 最右是停止键。先停掉这轮, 再测键盘 */
  const holdBusy = await p6.locator("#btn-hold").textContent();
  ok(await right() === "stop|danger|true" && holdBusy === "助理正在干活, 点停止可打断", "助理在跑: 最右是停止, 长条写助理在干活(不是'手机忙'): " + await right() + " / " + holdBusy);
  await p6.locator("#btn-send").click();
  await p6.waitForFunction(() => !busy(), null, { timeout: 10000 }).catch(() => {});
  await p6.locator("#btn-send").click(); await sleep(300);
  ok((await right()).startsWith("send|primary|false") && await p6.locator("#input").isVisible(), "点键盘: 回到文字输入, 有草稿所以最右是发送: " + await right());
  /* 重采样不许混叠(10-14 真机: "两分钟后"识别成"两分钟臭", 擦音能量在 8 kHz 以上): 48 kHz 下 1 kHz + 12 kHz 两个等幅纯音 → 页面的 micWav 降到 16 kHz,
     12 kHz 没有低通会折叠成 4 kHz。量 4 kHz 残留相对 1 kHz 的 dB(Goertzel), 要压到 -40 dB 以下 */
  const alias = await p6.evaluate(() => {
    const fs = 48000, n = fs / 2, x = new Float32Array(n);
    for (let i = 0; i < n; i++) x[i] = 0.4 * Math.sin(2 * Math.PI * 1000 * i / fs) + 0.4 * Math.sin(2 * Math.PI * 12000 * i / fs);
    const pcm = new Int16Array(micWav([x], fs, 16000), 44), m = pcm.length;
    const g = f => { let re = 0, im = 0; for (let i = 0; i < m; i++) { const a = 2 * Math.PI * f * i / 16000; re += pcm[i] * Math.cos(a); im -= pcm[i] * Math.sin(a); } return Math.hypot(re, im); };
    return 20 * Math.log10(g(4000) / g(1000));
  });
  ok(alias < -40, "48→16 kHz 重采样: 12 kHz 折叠到 4 kHz 的残留 " + alias.toFixed(1) + " dB(相对 1 kHz, 要 < -40)");
  await ctx6.close();
}

/* ---- 锁竖屏 + 禁缩放(10-16 用户: "不能双指放大, 横屏了内容也不能横屏"): iPhone 横放 → 整页转回竖的(<head> 里锁竖屏那段)。
   坐标换算 toPage 和浏览器自己算出来的 body 变换矩阵对账(两边独立: 一个是手推的公式, 一个是 CSS 实际生效的矩阵);
   转着点底栏、弹下拉菜单都得落在对的地方; 转回竖屏要撤干净。
   横放走 CDP 设屏幕角度: Playwright 的 "iPhone 13 landscape" 只把窗口改宽, screen.orientation 还报竖屏 0°(实测), 页面按角度判, 认不出横屏 ---- */
{
  const ctx7 = await browser.newContext({ ...devices["iPhone 13"], locale: "zh-CN" });
  try {   /* 这段抛错只记一条 FAIL: 直接抛出去会跳过文件末尾的收拾, 测试会话留在 ~/.ds4/assistant/sessions 里混进真对话(10-16 实撞) */
  const p7 = await ctx7.newPage(); p7.on("pageerror", e => console.log("pageerror:", e.message));
  await p7.goto(`http://127.0.0.1:${PORT}/#chat`); await login(p7);
  const cdp = await ctx7.newCDPSession(p7);
  const orient = async (w, h, type, angle) => { await cdp.send("Emulation.setDeviceMetricsOverride", { width: w, height: h, deviceScaleFactor: 3, mobile: true, screenWidth: w, screenHeight: h, screenOrientation: { type, angle } }); await sleep(300); };
  await orient(750, 342, "landscapePrimary", 90);
  const st = () => p7.evaluate(() => {
    const b = document.body.getBoundingClientRect(), tb = document.querySelector(".tabbar").getBoundingClientRect();
    const m = new DOMMatrix(getComputedStyle(document.body).transform).inverse();
    let err = 0;
    for (const [x, y] of [[10, 20], [300, 100], [innerWidth - 5, innerHeight - 5]]) { const q = m.transformPoint({ x, y }), p = toPage(x, y); err = Math.max(err, Math.abs(q.x - p.x), Math.abs(q.y - p.y)); }
    return { rot: document.documentElement.dataset.rot || "", shell: document.documentElement.dataset.shell, iw: innerWidth, ih: innerHeight, pw: pageW(), ph: pageH(),
      body: [b.left, b.top, b.width, b.height].map(Math.round), tab: [tb.left, tb.top, tb.width, tb.height].map(Math.round), err: +err.toFixed(2) };
  });
  let s = await st();
  ok(s.rot === "-90" && s.shell === "app" && s.pw === s.ih && s.ph === s.iw && s.body.join() === [0, 0, s.iw, s.ih].join() && s.err < 0.5,
    "开着时逆时针横放: 整页转 -90°, 页面宽高 = 屏幕高宽, 正好铺满屏幕, toPage 与实际变换矩阵一致: " + JSON.stringify(s));
  ok(s.tab[0] + s.tab[2] === s.iw && s.tab[3] === s.ih && s.tab[2] < 90, "底栏在手机底部 = 屏幕右边一条竖条: " + JSON.stringify(s.tab));
  /* 截图走 CDP: p7.screenshot 会先把窗口恢复成机型表的 390×664, 截的那一刻页面又竖回来了(下拉菜单也被 resize 关掉) */
  const shot7 = async n => { if (!SHOTS) return; mkdirSync(SHOTS, { recursive: true }); writeFileSync(path.join(SHOTS, n + ".png"), Buffer.from((await cdp.send("Page.captureScreenshot", { format: "png" })).data, "base64")); };
  await shot7("rot-landscape");
  await p7.reload(); await sleep(600); s = await st();
  ok(s.rot === "-90" && s.shell === "app" && s.body.join() === [0, 0, s.iw, s.ih].join() && s.err < 0.5, "横着拿直接打开: 第一帧就转好(<head> 里同步判): " + JSON.stringify(s));
  const vp = await p7.evaluate(() => [getComputedStyle(document.documentElement).touchAction, document.querySelector("meta[name=viewport]").content]);
  ok(vp[0] === "pan-x pan-y" && /user-scalable=no/.test(vp[1]) && /maximum-scale=1\b/.test(vp[1]), "禁缩放: 根元素只许拖动, viewport 不许缩放: " + JSON.stringify(vp));
  const tk = await p7.locator(".tabbar a[data-tab=tasks]").boundingBox();
  await p7.touchscreen.tap(tk.x + tk.width / 2, tk.y + tk.height / 2); await sleep(300);
  ok(await p7.evaluate(() => location.hash) === "#tasks", "转着点底栏'任务'(按屏幕坐标点): 进得去");
  await p7.evaluate(() => { location.hash = "models"; }); await sleep(400);   /* 模型页在"更多"里, 不在底栏; 它的"下载源"是常驻的下拉框 */
  await p7.evaluate(() => document.getElementById("m-ep").scrollIntoView({ block: "center" })); await sleep(300);   /* 在页面下半截, 转着的页面高 750 装不下 */
  const se = await p7.locator("#m-ep").boundingBox();
  await p7.touchscreen.tap(se.x + se.width / 2, se.y + se.height / 2); await sleep(300);
  const pk = await p7.evaluate(() => {
    const box = document.querySelector("#pick .pbox"), r = box.getBoundingClientRect(), a = pageRect(box), b = pageRect(document.getElementById("m-ep"));
    return { on: document.getElementById("pick").classList.contains("on"), scr: [r.left, r.top, r.right, r.bottom].map(Math.round), gapBelow: Math.round(a.top - b.bottom), gapAbove: Math.round(b.top - a.bottom), left: Math.round(a.left - b.left) };
  });
  ok(pk.on && pk.scr[0] >= 0 && pk.scr[1] >= 0 && pk.scr[2] <= s.iw && pk.scr[3] <= s.ih && (pk.gapBelow === 6 || pk.gapAbove === 6) && Math.abs(pk.left) <= 1,
    "转着弹下拉菜单: 在屏幕内, 贴着'下载源'框(页面坐标上下差 6 px、左对齐): " + JSON.stringify(pk));
  await shot7("rot-pick");
  await p7.evaluate(() => closePick());
  /* 转回竖屏: 撤掉旋转, 外壳还是 app, 底栏回到底部 */
  await orient(390, 664, "portraitPrimary", 0);
  s = await st();
  ok(s.rot === "" && s.shell === "app" && s.pw === s.iw && s.err < 0.5 && s.tab[1] + s.tab[3] === s.ih && s.tab[2] === s.iw, "竖过来: 不转, 底栏在底部横条: " + JSON.stringify(s));
  /* 顺时针横放(顶朝右): 转 +90°, 底栏到屏幕左边 */
  await orient(750, 342, "landscapeSecondary", 270);
  s = await st();
  ok(s.rot === "90" && s.body.join() === [0, 0, s.iw, s.ih].join() && s.err < 0.5 && s.tab[0] === 0 && s.tab[3] === s.ih, "顺时针横放: 整页转 +90°, 底栏在屏幕左边: " + JSON.stringify(s));
  /* 倒过来(10-16 小米实撞: 只管了横放, 倒置 180° 照样翻): 转 180°, 底栏到屏幕顶上 */
  await orient(390, 664, "portraitSecondary", 180);
  s = await st();
  ok(s.rot === "180" && s.pw === s.iw && s.ph === s.ih && s.body.join() === [0, 0, s.iw, s.ih].join() && s.err < 0.5 && s.tab[1] === 0 && s.tab[2] === s.iw,
    "倒过来: 整页转 180°, 底栏在屏幕顶上(= 手机底部): " + JSON.stringify(s));
  const tk2 = await p7.locator(".tabbar a[data-tab=chat]").boundingBox();
  await p7.touchscreen.tap(tk2.x + tk2.width / 2, tk2.y + tk2.height / 2); await sleep(300);
  ok(await p7.evaluate(() => location.hash) === "#chat", "倒着点底栏'助理': 进得去");
  await shot7("rot-180");
  } catch (e) { ok(false, "锁竖屏段抛错: " + e.message.split("\n")[0]); }
  await ctx7.close();
}
/* ---- 交易审批卡(docs/assistant_approval.md §3): 16 GB 的 Mac 没有云手机, 守门那半截在 ds4_asst_test 的 asst pay 段(真机 dumpsys 夹具);
   这里只验页面: 往测试会话的事件流里追加 tool + approval(字段与 asst_run.c wait_answer 发的一致), 重开页面看红卡。
   不点按钮: 运行线程并没在等, /approve 会回 409; 按钮走的 answer() 和别的审批卡是同一个函数, 上面已经点过 ---- */
{
  const ctx8 = await browser.newContext({ ...devices["iPhone 13"], locale: "zh-CN" });
  const shotRel = ".phone/pay-e2e.png", shotAbs = path.join(ASST, "workspace", shotRel);
  try {
    mkdirSync(path.dirname(shotAbs), { recursive: true }); writeFileSync(shotAbs, readFileSync(path.join(ROOT, "docs/img/monitor-en.png")));
    const args = JSON.stringify({ x: 360, y: 900 }), app = "com.tencent.mm/com.tencent.mm.plugin.remittance.ui.RemittanceUI";
    const ev = (id, pay) => JSON.stringify({ ev: "tool", id, name: "phone_tap", args }) + "\n" +
      JSON.stringify({ ev: "approval", id, name: "phone_tap", args, reason: "payment: payment page " + app, target: "phone", path: "", cls: "pay", pay, once_only: true, warn: true }) + "\n";
    appendFileSync(path.join(ASST, "sessions", sid, "events.jsonl"),
      ev("pay-e2e-1", { app, secure: false, screen: shotRel, at: [360, 900], hit: "", amounts: ["¥0.01"], frame_age_s: 2, unseen: false }) +
      ev("pay-e2e-2", { app: "com.tencent.mm/com.tencent.mm.framework.app.UIPageFragmentActivity", secure: true, screen: "", at: [360, 900], hit: "", amounts: [], frame_age_s: 40, unseen: true, win: "cf74994", before: { screen: shotRel, app, age_s: 4 } }));
    const p8 = await ctx8.newPage(); p8.on("pageerror", e => console.log("pageerror:", e.message));
    await p8.goto(`http://127.0.0.1:${PORT}/?session=${sid}#chat`); await login(p8);
    await p8.waitForFunction(() => document.querySelectorAll(".appr .paytag").length === 2, null, { timeout: 8000 });
    await p8.waitForFunction(() => { const i = [...document.querySelectorAll(".appr .payshot img")]; return i.length === 2 && i.every(x => x.complete && x.naturalWidth > 0 && x.getBoundingClientRect().height > 0); }, null, { timeout: 8000 });
    const c = await p8.evaluate(() => [...document.querySelectorAll(".appr .paytag")].map(tg => {
      const a = tg.closest(".appr"), img = a.querySelector(".payshot img"), d = a.querySelector(".payshot .dot");
      const im = img && img.getBoundingClientRect(), dr = d && d.getBoundingClientRect();
      return { tag: tg.textContent, amt: a.querySelector(".payamt").textContent, btns: [...a.querySelectorAll(".abtns button")].map(b => b.textContent), note: (a.querySelector(".abtns .muted") || {}).textContent || "",
               text: a.textContent, fit: !im || im.right <= a.getBoundingClientRect().right + 1, dot: dr ? [Math.round((dr.left + dr.width / 2 - im.left) / im.width * 720), Math.round((dr.top + dr.height / 2 - im.top) / im.height * 1280)] : null, h: im ? Math.round(im.height) : 0, vh: innerHeight };
    }));
    ok(c[0].tag === "付款 · com.tencent.mm" && c[0].amt === "屏幕上的金额: ¥0.01", "交易卡: 红头写前台包名, 金额原样: " + JSON.stringify([c[0].tag, c[0].amt]));
    ok(c[0].btns.join() === "仅这次,拒绝" && c[0].note.includes("每次都要你批"), "交易卡只有 仅这次 / 拒绝, 不给本会话/总是: " + JSON.stringify([c[0].btns, c[0].note]));
    ok(c[0].dot && Math.abs(c[0].dot[0] - 360) <= 8 && Math.abs(c[0].dot[1] - 900) <= 8 && c[0].h > 0 && c[0].h <= c[0].vh * 0.4 + 1 && c[0].fit, "现场截图上红圈落在要点的 (360,900), 图高不超 40vh、不出卡片: " + JSON.stringify(c[0].dot) + " h=" + c[0].h);
    ok(c[1].h > 0 && !c[1].dot && c[1].text.includes("禁止截屏") && c[1].text.includes("UIPageFragmentActivity") && c[1].text.includes("进这一页之前的最后一屏(4 秒前)") && c[1].text.includes("模型没看过这个屏幕(它上一眼是 40 秒前"),
      "安全窗口那张: 写明禁止截屏与页面名, 放进这页之前的最后一屏(不画红圈: 落点在看不见的那页上): " + c[1].text.slice(0, 160));
    ok(c[1].btns.join() === "仅这次,拒绝" && c[1].note.includes("批一次管这一页"), "安全窗口那张: 两键, 写明批一次管这一页: " + c[1].note);
    if (SHOTS) { mkdirSync(SHOTS, { recursive: true }); await p8.locator(".appr .paytag").first().scrollIntoViewIfNeeded(); await p8.screenshot({ path: path.join(SHOTS, "pay-card.png") }); }
  } catch (e) { ok(false, "交易卡段抛错: " + e.message.split("\n")[0]); }
  rmSync(shotAbs, { force: true });
  await ctx8.close();
}
/* ---- 网页预览(docs/assistant_page.md §5): 工作区里放一个会"越狱"的网页, 页面里调真的 fileCard() 点预览/打开。
   网页自己的脚本依次试: 读 localStorage(登录票在那) / 碰 parent.document / fetch 工作台的 /api / 加载同目录 js 与子目录图片, 结果写进 #r。
   要的是: 前三样全被挡、后两样能用(后者验 CSP 的 'self' + 显式主机在不透明源里照样认同主机)。"打开"的新标签页同一个响应头照样锁着 ---- */
{
  const ctx9 = await browser.newContext({ ...devices["iPhone 13"], locale: "zh-CN" });
  const dir = path.join(ASST, "workspace", "e2e-page");
  try {
    mkdirSync(path.join(dir, "img"), { recursive: true });
    writeFileSync(path.join(dir, "app.js"), "window.__app = true;\n");
    writeFileSync(path.join(dir, "img", "logo.png"), readFileSync(path.join(ROOT, "docs/img/monitor-en.png")));
    writeFileSync(path.join(dir, "index.html"), `<!doctype html><meta charset=utf-8><title>e2e 网页</title><h1>预览测试</h1><pre id=r>running</pre><img id=logo src="img/logo.png" width=40>
<script>const out = []; window.__app = false;
try { localStorage.getItem("ds4.studio.session"); out.push("ls:open"); } catch (e) { out.push("ls:blocked"); }
try { void parent.document.title; out.push(parent === window ? "parent:self" : "parent:open"); } catch (e) { out.push("parent:blocked"); }
out.push("origin:" + self.origin);</script>
<script src="app.js"></script>
<script>out.push(window.__app ? "js:loaded" : "js:missing");
fetch("/api/assistant/sessions").then(() => out.push("fetch:open"), () => out.push("fetch:blocked")).finally(() => {
  const im = document.getElementById("logo"), fin = () => { out.push(im.naturalWidth > 0 ? "img:loaded" : "img:missing"); document.getElementById("r").textContent = out.join(" "); };
  im.complete ? fin() : (im.onload = im.onerror = fin);
});</script>`);
    const p9 = await ctx9.newPage(); p9.on("pageerror", e => console.log("pageerror:", e.message));
    await p9.goto(`http://127.0.0.1:${PORT}/#chat`); await login(p9);
    await p9.evaluate(() => { const c = fileCard("e2e-page/index.html", {}); c.id = "pv-e2e"; document.body.appendChild(c); });
    const btns = await p9.evaluate(() => [...document.querySelectorAll("#pv-e2e button")].map(b => b.textContent));
    ok(btns.join() === "下载,预览", "网页卡: 下载 / 预览 两个按钮: " + JSON.stringify(btns));
    await p9.locator("#pv-e2e .pvbtn").click();
    await p9.waitForSelector("#pv-e2e iframe.pvframe", { timeout: 8000 });
    const fr = await p9.evaluate(() => { const f = document.querySelector("#pv-e2e iframe"), a = document.querySelector("#pv-e2e a.pvopen"); return { sb: f.getAttribute("sandbox"), src: f.getAttribute("src"), href: a && a.getAttribute("href"), tgt: a && a.target, open: a && a.textContent }; });
    ok(fr.sb === "allow-scripts allow-modals" && /^\/p\/[0-9a-f]{32}\/index\.html$/.test(fr.src) && fr.href === fr.src && fr.tgt === "_blank" && fr.open === "打开",
      "预览 = sandbox iframe 指 /p/<口令>/index.html, 旁边'打开'是 <a target=_blank>: " + JSON.stringify(fr));
    let frame = null; for (let i = 0; i < 40 && !frame; i++) { frame = p9.frames().find(f => f.url().includes("/p/")); if (!frame) await sleep(100); }
    await frame.waitForFunction(() => document.getElementById("r") && document.getElementById("r").textContent !== "running", null, { timeout: 8000 });
    const res = await frame.evaluate(() => document.getElementById("r").textContent);
    ok(["ls:blocked", "parent:blocked", "origin:null", "js:loaded", "fetch:blocked", "img:loaded"].every(x => res.split(" ").includes(x)),
      "iframe 里: 票读不到 / 碰不到父页 / 不透明源 / 同目录 js 与子目录图能加载 / fetch /api 被挡: " + res);
    const r = await fetch(`http://127.0.0.1:${PORT}${fr.href}`), csp = r.headers.get("content-security-policy") || "";
    ok(r.status === 200 && r.headers.get("content-type").startsWith("text/html") && csp.startsWith("sandbox allow-scripts allow-modals;") && csp.includes("connect-src 'none'") && r.headers.get("referrer-policy") === "no-referrer",
      "/p/ 直取: 200 text/html + CSP sandbox + connect-src 'none' + no-referrer: " + csp.slice(0, 60));
    const tab = await ctx9.newPage(); await tab.goto(`http://127.0.0.1:${PORT}${fr.href}`);
    await tab.waitForFunction(() => document.getElementById("r").textContent !== "running", null, { timeout: 8000 });
    const res2 = await tab.evaluate(() => document.getElementById("r").textContent);
    ok(["ls:blocked", "origin:null", "fetch:blocked", "img:loaded"].every(x => res2.split(" ").includes(x)), "'打开'的新标签页同样锁着(响应头生效, 不靠 iframe 属性): " + res2);
    /* Host 被中间层改掉时(穿透/反代把 Host 换成 127.0.0.1:8000), CSP 里的显式主机就对不上浏览器看到的地址, 只剩 'self' 撑着。
       起一个把 Host 改成 rewritten.invalid 的小代理, 从它打开同一页: 同目录 js 与图照样要能加载 */
    const { createServer: httpServer, request: httpReq } = await import("node:http");
    const px = httpServer((q, s) => { const u = httpReq({ host: "127.0.0.1", port: PORT, path: q.url, method: q.method, headers: { ...q.headers, host: "rewritten.invalid" } }, v => { s.writeHead(v.statusCode, v.headers); v.pipe(s); }); q.pipe(u); });
    await new Promise(r => px.listen(0, "127.0.0.1", r));
    const tab3 = await ctx9.newPage(); await tab3.goto(`http://127.0.0.1:${px.address().port}${fr.href}`);
    await tab3.waitForFunction(() => document.getElementById("r").textContent !== "running", null, { timeout: 8000 });
    const res3 = await tab3.evaluate(() => document.getElementById("r").textContent); px.close();
    ok(["ls:blocked", "fetch:blocked", "js:loaded", "img:loaded"].every(x => res3.split(" ").includes(x)), "Host 被改写(CSP 显式主机对不上)时只靠 'self' 资源照样加载: " + res3);
    const miss = await fetch(`http://127.0.0.1:${PORT}${fr.href.replace("index.html", "../top.html")}`);
    ok(miss.status === 404, "同一口令越出目录 → 404");
    if (SHOTS) { mkdirSync(SHOTS, { recursive: true }); await p9.locator("#pv-e2e").scrollIntoViewIfNeeded(); await p9.screenshot({ path: path.join(SHOTS, "page-preview.png") }); }
  } catch (e) { ok(false, "网页预览段抛错: " + e.message.split("\n")[0]); }
  rmSync(dir, { recursive: true, force: true });
  await ctx9.close();
}
await browser.close();

/* ---- 收拾 ---- */
for (const d of readdirSync(path.join(ASST, "trash", "skills"))) if (d.startsWith("e2e-weekly-report-")) rmSync(path.join(ASST, "trash", "skills", d), { recursive: true, force: true });
if (existsSync(path.join(ASST, "trash", "tasks"))) for (const f of readdirSync(path.join(ASST, "trash", "tasks"))) { try { if (JSON.parse(readFileSync(path.join(ASST, "trash", "tasks", f), "utf8")).text.startsWith("盯着 e2e 的价格")) rmSync(path.join(ASST, "trash", "tasks", f), { force: true }); } catch (_) {} }
cleanup();
const trash = path.join(ASST, "trash", "sessions"); mkdirSync(trash, { recursive: true });
for (const s of readdirSync(path.join(ASST, "sessions"))) if (!before.has(s)) renameSync(path.join(ASST, "sessions", s), path.join(trash, s + "-e2e-" + Date.now()));
rmSync(report, { force: true }); rmSync(pdf, { force: true }); rmSync(path.join(ASST, "workspace", "inbox", "合同.docx"), { force: true }); rmSync(path.join(ASST, "workspace", ".trash"), { recursive: true, force: true });
if (!hadAuth) rmSync(AUTH, { force: true });
console.log(fails ? `asst e2e: ${fails} FAILED` : "asst e2e: all passed");
process.exit(fails ? 1 : 0);
