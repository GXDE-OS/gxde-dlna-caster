#!/usr/bin/env python3
"""DLNA-Caster: 通过 DLNA/UPnP 协议, 把 Linux 桌面屏幕 + 声音 (或本地媒体文件)
投屏到同一局域网内的安卓电视 / 机顶盒 (需开启 DLNA 接收端)。

依赖: python3 (标准库) + ffmpeg; 桌面声音采集需要 PulseAudio (或 PipeWire 兼容层)。

用法示例:
  python3 main.py discover                       # 扫描局域网 DLNA 设备
  python3 main.py cast                           # 投屏整个桌面 + 声音
  python3 main.py cast --file video.mp4          # 投屏本地视频文件
  python3 main.py cast --preview                 # 只启动采集+流服务, 不发指令 (可用 VLC 预览)
"""

import argparse
import os
import subprocess
import sys
import threading
import time

from dlnacast import capture, server, soap, ssdp


def lan_ip():
    """获取本机局域网 IP, 优先返回 192.168.x / 10.x / 172.16-31.x 私网地址。"""
    import socket
    import struct
    import fcntl
    try:
        best = None
        for idx, name in socket.if_nameindex():
            try:
                addr = socket.inet_ntoa(fcntl.ioctl(
                    socket.socket(socket.AF_INET, socket.SOCK_DGRAM),
                    0x8915,  # SIOCGIFADDR
                    struct.pack("256s", name[:15].encode()))[20:24])
            except OSError:
                continue
            if addr == "127.0.0.1":
                continue
            first = addr.split(".")[0]
            if first in ("192", "10") or (first == "172" and 16 <= int(addr.split(".")[1]) <= 31):
                return addr
            if best is None:
                best = addr
        if best:
            return best
    except Exception:
        pass
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def cmd_discover(args):
    print(f"正在扫描局域网内的 DLNA 播放设备 (超时 {args.timeout}s) ...")
    renderers = ssdp.discover(timeout=args.timeout)
    if not renderers:
        print("未发现任何 DLNA 播放设备。")
        print("请确认: 电视/盒子与电脑在同一局域网, 且电视上已安装并开启 DLNA 接收端")
        print("(安卓电视一般自带媒体接收器; 也可用 VLC / Kodi / BubbleUPnP 等应用)。")
        sys.exit(1)
    for i, r in enumerate(renderers):
        print(f"[{i}] {r.name}\n    控制端点: {r.control_url}")
    return renderers


def select_renderer(renderers, args):
    if args.index is not None:
        if not 0 <= args.index < len(renderers):
            sys.exit(f"设备索引 {args.index} 无效")
        return renderers[args.index]
    if args.device:
        for r in renderers:
            if args.device.lower() in r.name.lower():
                return r
        sys.exit(f"未找到名称包含 '{args.device}' 的设备")
    if len(renderers) == 1:
        return renderers[0]
    print("检测到多个设备:")
    for i, r in enumerate(renderers):
        print(f"  [{i}] {r.name}")
    try:
        idx = int(input("请选择设备索引: ").strip())
    except (EOFError, ValueError):
        sys.exit("输入无效")
    if not 0 <= idx < len(renderers):
        sys.exit("设备索引无效")
    return renderers[idx]


def cmd_cast(args):
    if not capture.ffmpeg_available():
        sys.exit("未找到 ffmpeg, 请先安装: sudo apt install ffmpeg")
    if args.file and not os.path.isfile(args.file):
        sys.exit(f"文件不存在: {args.file}")
    if args.port < 1 or args.port > 65535:
        sys.exit("端口无效")

    renderer = None
    if not args.preview:
        renderers = cmd_discover(args)
        renderer = select_renderer(renderers, args)

    ip = args.address or lan_ip()
    stream_url = f"http://{ip}:{args.port}/stream"
    print(f"流地址: {stream_url}")

    httpd = server.StreamServer(("0.0.0.0", args.port))
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    cmd = capture.build_command(args)
    print("启动 ffmpeg 采集/编码 ...")
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, bufsize=0)
    httpd.pipe = proc.stdout

    if renderer is not None:
        print(f"投屏目标: {renderer.name}")
        try:
            soap.set_av_transport_uri(renderer.control_url, stream_url)
            soap.play(renderer.control_url)
            print("已向设备发送播放指令 (SetAVTransportURI + Play)。")
        except Exception as exc:
            proc.terminate()
            httpd.shutdown()
            sys.exit(f"向设备发送控制指令失败: {exc}")
    else:
        print("--preview 模式: 未向任何设备发送指令, 可在 VLC / 浏览器中打开上面的流地址预览。")

    print("投屏中... 按 Ctrl+C 停止。")
    try:
        while True:
            time.sleep(1)
            if proc.poll() is not None:
                err = proc.stderr.read().decode("utf-8", "replace")
                print(f"ffmpeg 已退出 (code {proc.returncode})")
                if err.strip():
                    print(err)
                break
    except KeyboardInterrupt:
        pass
    finally:
        if renderer is not None:
            try:
                soap.stop(renderer.control_url)
            except Exception:
                pass
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        httpd.shutdown()
        print("\n已停止投屏。")


def main():
    parser = argparse.ArgumentParser(
        prog="dlnacaster",
        description="通过 DLNA 协议把桌面屏幕/声音或本地媒体投屏到电视。")
    sub = parser.add_subparsers(dest="command", required=True)

    sp = sub.add_parser("discover", help="扫描局域网内的 DLNA 播放设备")
    sp.add_argument("--timeout", type=int, default=6, help="扫描超时秒数 (默认 6)")
    sp.set_defaults(func=cmd_discover)

    sp = sub.add_parser("cast", help="开始投屏")
    sp.add_argument("--file", help="投屏本地媒体文件 (默认投屏整个桌面)")
    sp.add_argument("--preview", action="store_true",
                    help="只启动采集和流服务, 不向设备发指令 (用于本地预览/调试)")
    sp.add_argument("--device", help="按名称关键字选择设备")
    sp.add_argument("--index", type=int, help="按序号选择设备 (见 discover 输出)")
    sp.add_argument("--address", help="本机对外 IP (默认自动检测)")
    sp.add_argument("--port", type=int, default=8090, help="流服务端口 (默认 8090)")
    sp.add_argument("--no-audio", action="store_true", help="不采集/不发送声音")
    sp.add_argument("--audio-source", help="PulseAudio 音频源 (默认自动检测桌面音频)")
    sp.add_argument("--fps", type=int, default=30, help="屏幕采集帧率 (默认 30)")
    sp.add_argument("--scale", type=int, default=1080,
                    help="画面高度上限, 0 表示不缩放 (默认 1080)")
    sp.add_argument("--bitrate", default="4M", help="视频码率, 如 4M / 2000k")
    sp.add_argument("--audio-bitrate", default="128k", help="音频码率")
    sp.add_argument("--timeout", type=int, default=6, help="设备扫描超时秒数 (默认 6)")
    sp.set_defaults(func=cmd_cast)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
