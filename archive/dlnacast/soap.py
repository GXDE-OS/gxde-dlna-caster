"""UPnP SOAP 控制点: 通过 SOAP/XML 向设备的 AVTransport 服务发送控制指令。"""

import urllib.request
from xml.sax.saxutils import escape

SOAP_ENV = "http://schemas.xmlsoap.org/soap/envelope/"
AV_TRANSPORT = "urn:schemas-upnp-org:service:AVTransport:1"


def _call(control_url, service_type, action, args):
    """发送一个 SOAP 动作, 返回 (HTTP 状态码, 响应正文)。"""
    arg_xml = "".join(f"<{k}>{escape(str(v))}</{k}>" for k, v in args.items())
    body = (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        f'<s:Envelope xmlns:s="{SOAP_ENV}" '
        's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
        f'<s:Body><u:{action} xmlns:u="{service_type}">{arg_xml}</u:{action}></s:Body>'
        "</s:Envelope>"
    )
    req = urllib.request.Request(control_url, data=body.encode("utf-8"), method="POST")
    req.add_header("Content-Type", 'text/xml; charset="utf-8"')
    req.add_header("SOAPAction", f'"{service_type}#{action}"')
    req.add_header("User-Agent", "DLNA-Caster/1.0")
    req.add_header("Connection", "close")
    with urllib.request.urlopen(req, timeout=15) as resp:
        return resp.status, resp.read().decode("utf-8", "replace")


def set_av_transport_uri(control_url, uri, metadata=None):
    """告诉设备把播放源切换到 uri。metadata 为 DIDL-Lite XML, None 时自动生成。"""
    if metadata is None:
        metadata = (
            '<DIDL-Lite xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/" '
            'xmlns:dc="http://purl.org/dc/elements/1.1/" '
            'xmlns:upnp="urn:schemas-upnp-org:metadata-1-0/upnp/">'
            '<item id="0" parentID="-1" restricted="1">'
            "<dc:title>DLNA-Caster 投屏</dc:title>"
            f'<res protocolInfo="http-get:*:video/mp2t:*">{uri}</res>'
            "</item></DIDL-Lite>"
        )
    return _call(control_url, AV_TRANSPORT, "SetAVTransportURI", {
        "InstanceID": "0",
        "CurrentURI": uri,
        "CurrentURIMetaData": metadata,
    })


def play(control_url, speed=1):
    """开始播放。"""
    return _call(control_url, AV_TRANSPORT, "Play", {
        "InstanceID": "0",
        "Speed": str(speed),
    })


def stop(control_url):
    """停止播放。"""
    return _call(control_url, AV_TRANSPORT, "Stop", {"InstanceID": "0"})


def get_transport_info(control_url):
    """查询当前播放状态。"""
    return _call(control_url, AV_TRANSPORT, "GetTransportInfo", {"InstanceID": "0"})
