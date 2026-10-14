# ashmem 树外内核模块(云手机用, 方案稿 docs/assistant_phone.md): 老 Chromium 分支的 WebView(淘宝闪购/饿了么的 UC 内核等)只认 /dev/ashmem 的 ioctl,
# 不认 memfd; 主线 5.18 删了 ashmem, 宿主(Ubuntu 6.17)没有 ⇒ 这类 app 启动 6 s 必死(10-14 实撞, 见 fable5)。
# 源 = Linux 5.17 drivers/staging/android/ashmem.c(GPL-2.0)最后一版, 移植点都标了 "ds4 6.17 移植"。
#   make            编(要 linux-headers-$(uname -r))
#   make load       sudo insmod(一次性; 永久装进 modules-load 走 scripts/phone_prepare_linux.sh)
# 出错会怎样: 缺内核头 → "No rule to make target" 指向 /lib/modules/<ver>/build; 内核再改接口 → 在标了移植点的几处改。
obj-m := ashmem_linux.o
ashmem_linux-y := ashmem.o
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
load: all
	sudo insmod ashmem_linux.ko && ls -l /dev/ashmem
.PHONY: all clean load
