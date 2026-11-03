# StackChan 开发快捷命令
# 把每天重复的动作（编译/烧录/监视/部署/验证）收成一条命令

SHELL   := /bin/bash
FW      := firmware/chan-cores3
PORT    ?=
IP      ?=
OTA_PASS ?= stackchan

# 清掉 WorkBuddy 注入的 PYTHONPATH 与本机代理：
#   前者会让 pio 的 python 解析错（还会缺 click），所以显式指回 pylib
#   后者让 PlatformIO 源慢到超时（11.7s vs 1.8s）
PIO_ENV := env -u HTTP_PROXY -u HTTPS_PROXY -u http_proxy -u https_proxy -u ALL_PROXY -u all_proxy PYTHONPATH="$(HOME)/.platformio-pylib"

# 串口自动探测
DETECT_PORT = $(shell ls /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null | head -1)
P           = $(if $(PORT),$(PORT),$(DETECT_PORT))

.PHONY: help build flash flash-ota monitor port clean deploy deploy-nginx health verify recs rec

help:
	@echo "StackChan 常用命令"
	@echo "  make build           编译主固件 (chan-cores3)"
	@echo "  make flash           烧录（自动探测串口，USB）"
	@echo "  make flash-ota IP=x  无线烧录（设备已联网）"
	@echo "  make monitor         串口监视器 @115200"
	@echo "  make port            显示当前探测到的串口"
	@echo "  make clean           清理构建产物"
	@echo "  make deploy          部署网关到阿里云 ECS（含热词表 terms.txt）"
	@echo "  make recs            列出设备上传的会议录音"
	@echo "  make rec             下载最新一段录音到 /tmp/chan-last.wav"
	@echo "  make web             打开手机端会议记录页（历史列表）"
	@echo "  make deploy-nginx    更新 ECS nginx 反代配置"
	@echo "  make health          查线上网关健康状态"
	@echo "  make verify WAV=x    百炼直连验证一段音频"
	@echo ""
	@echo "指定串口：make flash PORT=/dev/cu.usbmodem1101"
	@echo "无线烧录：make flash-ota IP=192.168.1.23（口令改 OTA_PASS=xxx）"

build:
	@echo ">> 编译 $(FW)"
	@cd $(FW) && $(PIO_ENV) pio run

flash:
	@test -n "$(P)" || { echo "未找到串口。插上设备，或 make flash PORT=/dev/cu.xxx"; exit 1; }
	@echo ">> 烧录到 $(P)"
	@cd $(FW) && $(PIO_ENV) pio run -t upload --upload-port $(P)

# 无线烧录：设备必须已连上 WiFi 且跑着带 OTA 的固件
# 首次仍然只能 USB，因为接收端本身要靠 USB 装进去
flash-ota:
	@test -n "$(IP)" || { echo "用法：make flash-ota IP=192.168.1.23（屏幕顶部会显示 IP）"; exit 1; }
	@echo ">> OTA 烧录到 $(IP)"
	@cd $(FW) && $(PIO_ENV) PLATFORMIO_UPLOAD_FLAGS="--auth=$(OTA_PASS)" PLATFORMIO_UPLOAD_PROTOCOL=espota pio run -t upload --upload-port $(IP)

monitor:
	@test -n "$(P)" || { echo "未找到串口。插上设备，或 make monitor PORT=/dev/cu.xxx"; exit 1; }
	@cd $(FW) && $(PIO_ENV) pio device monitor -p $(P) -b 115200

port:
	@echo "$(if $(P),$(P),未探测到串口)"

clean:
	rm -rf $(FW)/.pio
	@echo ">> 已清理 $(FW)/.pio"

deploy:
	scp gateway/app.py gateway/meeting.py aliyun:/opt/meeting-gw/
	scp assets/terms.txt aliyun:/opt/meeting-gw/terms.txt
	ssh aliyun 'systemctl restart meeting-gw && sleep 1 && systemctl is-active meeting-gw'

# ---- 本机私有配置：secrets.mk（已 gitignore，不进仓库）----
#   GW        网关根地址    http://<公网IP>/asr
#   TK        设备令牌      = 服务器 .env 的 DEVICE_TOKEN
#   WEB_BASE  手机端页面根  http://<公网IP>/m
#   WEB_KEY   手机端访问密钥 = 服务器 .env 的 WEB_KEY（也是二维码里那一段）
GW       ?= http://127.0.0.1/asr
TK       ?=
WEB_BASE ?= http://127.0.0.1/m
WEB_KEY  ?=
-include secrets.mk

# 看设备上传的音频：列出 / 下载最新一段（用来听清楚到底录成什么样）
recs:
	@curl -s --noproxy '*' --max-time 10 "$(GW)/recordings?token=$(TK)" || echo "网关不通"

rec:
	@curl -s --noproxy '*' --max-time 10 "$(GW)/recordings?token=$(TK)" -o /tmp/chan-recs.json
	@name=$$(python3 -c "import json;d=json.load(open('/tmp/chan-recs.json'));print(d['items'][0]['name'] if d.get('items') else '')"); \
	if [ -z "$$name" ]; then echo "还没有录音文件"; exit 0; fi; \
	curl -s --noproxy '*' -o /tmp/chan-last.wav "$(GW)/recordings/$$name?token=$(TK)" && \
	echo "已下载 /tmp/chan-last.wav（$$name），可用 open /tmp/chan-last.wav 听"

deploy-nginx:
	scp gateway/nginx-meeting-gw.conf aliyun:/etc/nginx/conf.d/meeting-gw.conf
	ssh aliyun 'nginx -t && nginx -s reload'

health:
	@curl -s --noproxy '*' --max-time 10 "$(GW)/health" || echo "网关不通"

# 手机端会议记录页：扫设备结束页的二维码进来，也可以直接点这个链接
web:
	@echo "$(WEB_BASE)/$(WEB_KEY)"
	@open "$(WEB_BASE)/$(WEB_KEY)" 2>/dev/null || true

verify:
	@test -n "$(WAV)" || { echo "用法：make verify WAV=/tmp/asr_test.wav"; exit 1; }
	python3 scripts/dashscope_asr_verify.py $(WAV)
