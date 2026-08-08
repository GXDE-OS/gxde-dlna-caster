"""SSDP 发现局域网内的 DLNA/UPnP 媒体渲染设备 (MediaRenderer)。"""

import socket
import time
import urllib.request
from urllib.parse import urljoin
from xml.etree import ElementTree

SSDP_ADDR = "239.255.255.250"
SSDP_PORT = 1900
ST_MEDIARENDERER = "urn:schemas-upnp-org:device:MediaRenderer:1"
USER_AGENT = "DLNA-Caster/1.0"


class Renderer:
    """一个可用的 DLNA 播放设备。"""

    def __init__(self, name, location, control_url, event_url="", udn=""):
        self.name = name
        self.location = location
        self.control_url = control_url  # AVTransport 服务控制端点
        self.event_url = event_url
        self.udn = udn

    def __repr__(self):
        return f"<Renderer {self.name!r} {self.control_url}>"


def _strip_ns(tag):
    return tag.rsplit("}", 1)[-1]


def _find_text(elem, tag_name):
    """在 elem 子树里找第一个名为 tag_name 的元素的文本。"""
    for child in elem.iter():
        if _strip_ns(child.tag) == tag_name:
            return (child.text or "").strip()
    return ""


def _fetch_description(location):
    """拉取设备描述 XML。"""
    try:
        req = urllib.request.Request(location, headers={"User-Agent": USER_AGENT})
        with urllib.request.urlopen(req, timeout=5) as resp:
            return ElementTree.fromstring(resp.read())
    except Exception:
        return None


def discover(timeout=6, st=ST_MEDIARENDERER):
    """发送 SSDP M-SEARCH, 收集响应并解析设备描述, 返回 Renderer 列表。"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.settimeout(1)
    try:
        msg = (
            "M-SEARCH * HTTP/1.1\r\n"
            f"HOST: {SSDP_ADDR}:{SSDP_PORT}\r\n"
            'MAN: "ssdp:discover"\r\n'
            "MX: 3\r\n"
            f"ST: {st}\r\n"
            f"USER-AGENT: {USER_AGENT}\r\n"
            "\r\n"
        ).encode("ascii")
        sock.sendto(msg, (SSDP_ADDR, SSDP_PORT))
    except OSError as exc:
        sock.close()
        raise RuntimeError(f"SSDP 报文发送失败: {exc}")

    locations = set()
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            data, _ = sock.recvfrom(4096)
        except socket.timeout:
            continue
        text = data.decode("utf-8", "replace")
        for line in text.splitlines():
            key, sep, value = line.partition(":")
            if sep and key.strip().lower() == "location":
                locations.add(value.strip())
                break
    sock.close()

    renderers = []
    for loc in sorted(locations):
        root = _fetch_description(loc)
        if root is None:
            continue
        device = next((e for e in root.iter() if _strip_ns(e.tag) == "device"), None)
        if device is None:
            continue
        if "MediaRenderer" not in _find_text(device, "deviceType"):
            continue

        control_url = ""
        event_url = ""
        for service in device.iter():
            if _strip_ns(service.tag) != "service":
                continue
            if "AVTransport" in _find_text(service, "serviceType"):
                control_url = _find_text(service, "controlURL")
                event_url = _find_text(service, "eventSubURL")
                break
        if not control_url:
            continue

        renderers.append(Renderer(
            name=_find_text(device, "friendlyName") or loc,
            location=loc,
            control_url=urljoin(loc, control_url),
            event_url=urljoin(loc, event_url) if event_url else "",
            udn=_find_text(device, "UDN"),
        ))
    return renderers
