/* e2e_device.mjs — 助理"进 → 干 → 出"的页面端到端门(总纲 docs/assistant_arch.md §10), make asst-e2e 跑。
 * 起真的 ds4-train(从 18000 起找空端口) + 假模型 tests/asst/fake/ds4-server(主进程端口 +1, 按对话里 tool 结果数走: 读剪贴板 → 写报告 → locate → run → 五个交出去的动作 → 终稿),
 * 用系统 Chrome(Playwright channel=chrome)仿真 iPhone(触屏 + 定位 + 剪贴板权限)跑一遍, 逐项核对:
 *   1 事实行: messages.jsonl 第一条 user 以 "[设备: 手机 · … · 可用: 定位/…/打开app]" 开头, 没有位置(位置是 locate 工具); 第二条消息带 "上次定位: …" 与 "[附件: …]"
 *   2 剪贴板 / 定位: 设备卡自动做, tool 结果 = 预放进剪贴板的文字 / "lat,lng: 30.274100,120.155100 (±35 m)"; 报告里有剪贴板内容; meta.loc 落盘
 *   3 run: 沙盒里跑、不审批(decision=auto), 生成的文件在工作区
 *   4 文件卡只在 share(path) 时出: 下载得到的字节 == 盘上文件; 没有终稿卡; write_file 不出卡
 *   5 动作按钮: navigate / call / open_app 是真链接(iPhone UA 由页面自己定成 maps:// / tel: / snssdk1128://; 安卓 UA 的导航是 intent:geo + 高德网页兜底); copy_text 点后剪贴板 == 文本; share 带 text + 文件
 *   6 工具表协商: 第一次请求的 tools 里有 locate/read_clipboard/share/navigate/open_app/run; 电脑外壳 + 不安全上下文那次没有 clipboard/share/geolocation
 *   7 http 提示: 用局域网 IP 打开(不安全上下文)输入框上方出提示
 * 为什么不等页面进 serving 态: macOS 上主进程不扫进程表, 认不出外来的假模型; 这里直接调页面的 send()(发消息那条路照样走设备信息与能力表), serving 门是 train_model 的事, 另有单测。
 * 跑完: 杀两个进程; 测试建的会话挪进 ~/.ds4/assistant/trash/sessions/(与页面删除同路), 报告文件与 inbox 删掉; 本来没有登录文件的话把测试设的那份删掉。 */
import { spawn, execSync } from "node:child_process";
import { existsSync, readFileSync, readdirSync, renameSync, rmSync, mkdirSync, statSync, writeFileSync } from "node:fs";
import { homedir } from "node:os";
import { createServer } from "node:net";
import path from "node:path";

/* 端口自己找空的(主进程用 PORT, 假模型用 PORT+1): 写死 18000 时本机正挂着 ssh -L 18000:…:8000 spark 隧道,
   ds4-train 绑 0.0.0.0:18000 照样成功(macOS 上 127.0.0.1 那个更具体的绑定优先), 测试就连到了 spark 上的真工作台, 报"用户名或密码不对", 看着像登录坏了。 */
const free = p => new Promise(res => { const s = createServer().once("error", () => res(false)).listen(p, "127.0.0.1", () => s.close(() => res(true))); });
let PORT = 18000; while (!(await free(PORT)) || !(await free(PORT + 1))) PORT += 10;
const ROOT = process.cwd(), HOME = homedir();
const ASST = path.join(HOME, ".ds4", "assistant"), AUTH = path.join(HOME, ".ds4", "studio", "auth");
const pwDir = process.argv[2] || readdirSync(path.join(HOME, ".npm", "_npx")).map(d => path.join(HOME, ".npm", "_npx", d, "node_modules", "playwright")).find(existsSync);
if (!pwDir) { console.error("找不到 playwright 包; 给路径当第一个参数, 或 npx playwright 一次让它进缓存"); process.exit(2); }
const { chromium, devices } = await import(path.join(pwDir, "index.mjs"));

let fails = 0; const ok = (c, m) => { console.log((c ? "ok   " : "FAIL ") + m); if (!c) fails++; };
const sleep = ms => new Promise(r => setTimeout(r, ms));
const hadAuth = existsSync(AUTH);
const before = new Set(existsSync(path.join(ASST, "sessions")) ? readdirSync(path.join(ASST, "sessions")) : []);
const fake = spawn("tests/asst/fake/ds4-server", [String(PORT + 1)], { stdio: ["ignore", "inherit", "inherit"] });
const train = spawn("./ds4-train", ["--port", String(PORT), "--host", "0.0.0.0", "--root", ROOT], { stdio: ["ignore", "ignore", "inherit"] });
const cleanup = () => { try { fake.kill(); } catch (_) {} try { train.kill(); } catch (_) {} };
process.on("exit", cleanup);
for (let i = 0; i < 50; i++) { try { if ((await fetch(`http://127.0.0.1:${PORT}/manifest.webmanifest`)).ok) break; } catch (_) {} await sleep(200); }

const browser = await chromium.launch({ channel: "chrome", headless: true });
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
/* 文件卡: 只有 share(path) 那一张; write_file 不出卡; 没有终稿卡 */
await page.waitForSelector(".deliver", { timeout: 5000 }).catch(() => {});   /* 渲染有 90 ms 合并窗口, view 里有 end 时 DOM 可能还差一拍 */
const cards = { deliver: await page.locator(".deliver").count(), final: await page.locator("#chat .final").count() };
ok(cards.deliver === 1 && cards.final === 0, "文件卡只在 share(path) 出一张, 没有终稿卡: " + JSON.stringify(cards));
const dl = page.waitForEvent("download", { timeout: 8000 });
await page.locator(".deliver button", { hasText: "下载" }).first().click();
const d = await dl; const dlPath = await d.path();
ok(dlPath && readFileSync(dlPath).equals(readFileSync(report)), "下载字节与盘上文件一致");
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
await browser.close();

/* ---- 收拾 ---- */
cleanup();
const trash = path.join(ASST, "trash", "sessions"); mkdirSync(trash, { recursive: true });
for (const s of readdirSync(path.join(ASST, "sessions"))) if (!before.has(s)) renameSync(path.join(ASST, "sessions", s), path.join(trash, s + "-e2e-" + Date.now()));
rmSync(report, { force: true }); rmSync(pdf, { force: true }); rmSync(path.join(ASST, "workspace", "inbox", "合同.docx"), { force: true }); rmSync(path.join(ASST, "workspace", ".trash"), { recursive: true, force: true });
if (!hadAuth) rmSync(AUTH, { force: true });
console.log(fails ? `asst e2e: ${fails} FAILED` : "asst e2e: all passed");
process.exit(fails ? 1 : 0);
