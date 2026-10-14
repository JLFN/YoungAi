/* 语言切换 + 代码块复制 + 指南页目录高亮。不用框架: 网站就两页。 */
(function () {
  var root = document.documentElement;
  var KEY = "youngai.lang";

  function saved() { try { return localStorage.getItem(KEY); } catch (e) { return null; } }
  function save(l) { try { localStorage.setItem(KEY, l); } catch (e) { /* 隐私模式写不了, 只是下次不记得 */ } }

  function apply(l) {
    root.setAttribute("data-l", l);
    root.lang = l === "zh" ? "zh-CN" : "en";
    var t = document.querySelector("title");
    if (t && t.dataset[l]) t.textContent = t.dataset[l];
    document.querySelectorAll(".lang-btn").forEach(function (b) { b.textContent = l === "zh" ? "EN" : "中文"; });
  }

  // 优先 ?lang=en 链接参数, 其次上次的选择, 最后看浏览器语言
  var q = new URLSearchParams(location.search).get("lang");
  var l = q === "en" || q === "zh" ? q : saved() || ((navigator.language || "").toLowerCase().indexOf("zh") === 0 ? "zh" : "en");
  apply(l);

  document.addEventListener("DOMContentLoaded", function () {
    apply(root.getAttribute("data-l"));
    document.querySelectorAll(".lang-btn").forEach(function (b) {
      b.addEventListener("click", function () {
        var n = root.getAttribute("data-l") === "zh" ? "en" : "zh";
        apply(n); save(n);
      });
    });

    document.querySelectorAll("pre").forEach(function (pre) {
      var b = document.createElement("button");
      b.className = "copy"; b.type = "button"; b.textContent = "Copy";
      b.addEventListener("click", function () {
        // 每个 pre 里都包一个 code; 只取它的文字, 不然会把按钮上的 "Copy" 也复制进去
        var text = (pre.querySelector("code") || pre).innerText;
        navigator.clipboard && navigator.clipboard.writeText(text).then(function () {
          b.textContent = "Copied"; setTimeout(function () { b.textContent = "Copy"; }, 1400);
        });
      });
      pre.appendChild(b);
    });

    var links = Array.prototype.slice.call(document.querySelectorAll(".toc a[href^='#']"));
    if (links.length && "IntersectionObserver" in window) {
      var byId = {};
      links.forEach(function (a) { byId[a.getAttribute("href").slice(1)] = a; });
      var io = new IntersectionObserver(function (es) {
        es.forEach(function (e) {
          if (!e.isIntersecting) return;
          links.forEach(function (a) { a.classList.remove("on"); });
          var a = byId[e.target.id]; if (a) a.classList.add("on");
        });
      }, { rootMargin: "-80px 0px -70% 0px" });
      Object.keys(byId).forEach(function (id) { var s = document.getElementById(id); if (s) io.observe(s); });
    }
  });
})();
