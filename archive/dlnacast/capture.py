"""ffmpeg 采集: 抓取桌面屏幕 + 桌面音频, 或转码本地媒体文件, 输出 H.264/AAC 的 MPEG-TS 到 stdout。"""

import os
import shutil
import subprocess


def ffmpeg_available():
    return shutil.which("ffmpeg") is not None


def detect_monitor_source():
    """尝试用 pactl 找到桌面音频的 monitor 源 (系统声音)。找不到则返回 None。"""
    try:
        out = subprocess.run(
            ["pactl", "list", "sources", "short"],
            capture_output=True, text=True, timeout=5,
        ).stdout
    except Exception:
        return None
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) >= 2 and ".monitor" in parts[1]:
            return parts[1]
    return None


def _double_rate(rate):
    """把 '4M' 变成 '8M', 把 '2000k' 变成 '4000k'。"""
    rate = rate.strip()
    return f"{int(float(rate[:-1])) * 2}{rate[-1]}" if len(rate) > 1 else rate


def build_command(args):
    """根据参数构建 ffmpeg 命令行, 返回参数列表。

    args 需要包含: file, fps, scale, bitrate, audio_bitrate, no_audio, audio_source
    """
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "warning", "-y"]

    if args.file:
        # 投屏本地媒体文件
        cmd += ["-re", "-i", args.file]
    else:
        # 实时抓取桌面
        display = os.environ.get("DISPLAY")
        if not display:
            raise SystemExit(
                "未检测到 X11 显示环境 (DISPLAY 为空), 无法采集桌面。\n"
                "可以改用 --file 投屏本地媒体文件。"
            )
        cmd += ["-f", "x11grab", "-framerate", str(args.fps),
                "-draw_mouse", "1", "-i", display]
        if not args.no_audio:
            src = args.audio_source or detect_monitor_source() or "default"
            cmd += ["-f", "pulse", "-i", src]

    if args.scale:
        cmd += ["-vf", f"scale=-2:{args.scale}"]

    # H.264 + AAC, 编码为 MPEG-TS
    cmd += [
        "-c:v", "libx264", "-preset", "veryfast", "-tune", "zerolatency",
        "-pix_fmt", "yuv420p", "-b:v", args.bitrate,
        "-maxrate", args.bitrate, "-bufsize", _double_rate(args.bitrate),
        "-g", "30", "-keyint_min", "30", "-sc_threshold", "0",
    ]
    if args.no_audio:
        cmd += ["-an"]
    else:
        cmd += ["-c:a", "aac", "-b:a", args.audio_bitrate, "-ar", "44100", "-ac", "2"]

    cmd += ["-f", "mpegts", "-mpegts_flags", "+resend_headers", "-"]
    return cmd
