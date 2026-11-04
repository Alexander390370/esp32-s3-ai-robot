# -*- coding: utf-8 -*-
"""
一键启动：broker → PC 大脑

用法：
    python start.py                # 正常启动
    python start.py --no-broker    # broker 已在跑
"""

import argparse
import os
import socket
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).parent
BROKER_CONF = HERE / "broker" / "mosquitto.conf"
BROKER_PORT = 1884

MOSQUITTO = [
    r"C:\Program Files\Mosquitto\mosquitto.exe",
    r"C:\Program Files (x86)\Mosquitto\mosquitto.exe",
    "/usr/sbin/mosquitto",
    "mosquitto",
]

_children = []


def log(m):
    print(f"[{datetime.now():%H:%M:%S}] {m}", flush=True)


def port_open(port, host="127.0.0.1"):
    try:
        s = socket.create_connection((host, port), timeout=1)
        s.close()
        return True
    except Exception:
        return False


def ensure_broker_conf():
    BROKER_CONF.parent.mkdir(parents=True, exist_ok=True)
    if BROKER_CONF.exists():
        return
    BROKER_CONF.write_text(
        "# 本地 MQTT broker\n"
        f"listener {BROKER_PORT} 0.0.0.0\n"
        "allow_anonymous true\n"
        "max_packet_size 1048576\n"
        "max_queued_messages 100\n"
        "persistence false\n",
        encoding="utf-8",
        newline="\n",
    )
    log(f"已生成 {BROKER_CONF}")


def start_broker():
    if port_open(BROKER_PORT):
        log(f"broker 已在 :{BROKER_PORT}")
        return
    ensure_broker_conf()

    exe = next((c for c in MOSQUITTO if Path(c).exists()), None)
    if exe is None:
        exe = "mosquitto"          # 交给 PATH

    log("启动 MQTT broker...")
    kwargs = {"stdout": subprocess.DEVNULL, "stderr": subprocess.DEVNULL}
    if os.name == "nt":
        kwargs["creationflags"] = subprocess.CREATE_NO_WINDOW
    p = subprocess.Popen([exe, "-c", str(BROKER_CONF)], **kwargs)
    _children.append(("mosquitto", p))

    for _ in range(12):
        time.sleep(0.5)
        if port_open(BROKER_PORT):
            log(f"broker 就绪 0.0.0.0:{BROKER_PORT}")
            return
    log("broker 启动失败，检查 Mosquitto 是否安装")


def lan_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        try:
            return socket.gethostbyname(socket.gethostname())
        except Exception:
            return "127.0.0.1"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-broker", action="store_true")
    args = ap.parse_args()

    print("=" * 58)
    print(" 机器人 PC 端大脑")
    print("=" * 58)
    print()

    if not args.no_broker:
        start_broker()

    ip = lan_ip()
    print()
    print("=" * 58)
    print(f" 手机浏览器打开: http://{ip}:8765")
    print(" 手机和电脑要连同一个 WiFi")
    print(" 机器人固件里 MQTT_HOST 填: " + ip)
    print("=" * 58)
    print()

    brain = HERE / "pc" / "brain.py"
    p = subprocess.Popen([sys.executable, str(brain)],
                         cwd=str(HERE / "pc"))
    _children.append(("brain", p))

    try:
        p.wait()
    except KeyboardInterrupt:
        print()
        log("退出")
    finally:
        for name, proc in reversed(_children):
            if proc.poll() is None:
                try:
                    proc.terminate()
                    proc.wait(timeout=4)
                    log(f"已停止 {name}")
                except Exception:
                    pass


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    main()
