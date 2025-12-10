#!/usr/bin/env python3
import gi
gi.require_version("Gst", "1.0")
from gi.repository import Gst, GLib

Gst.init(None)

pipeline = None
webrtcbin = None


# === WebRTC Offer 回调 ===
def on_offer_created(webrtc, promise, _):
    reply = promise.get_reply()
    offer = reply.get_value("offer")

    print("\n=====  SDP OFFER  =====\n")
    print(offer.sdp.as_text())
    print("=======================\n")

    webrtc.emit("set-local-description", offer, None)


def on_negotiation_needed(webrtc, _):
    print("WebRTC negotiation-needed triggered.")
    promise = Gst.Promise.new_with_change_func(on_offer_created, webrtc, None)
    webrtc.emit("create-offer", None, promise)


# === rtph264pay pad-added → 链接到 webrtcbin ===
def on_rtph264pay_pad_added(pay, pad, user_data):
    print("[INFO] rtph264pay pad-added, linking to webrtcbin…")

    webrtc = user_data
    sinkpad = webrtc.get_request_pad("sink_%u")

    if pad.link(sinkpad) == Gst.PadLinkReturn.OK:
        print("[OK] Linked rtph264pay → webrtcbin")
    else:
        print("[ERROR] Failed linking rtph264pay → webrtcbin")

    sinkpad.unref()


# === 构建完整 Pipeline ===
def build_pipeline():
    global pipeline, webrtcbin

    pipeline = Gst.Pipeline.new("webrtc_record_pipeline")

    # Camera
    src = Gst.ElementFactory.make("libcamerasrc", None)
    convert = Gst.ElementFactory.make("videoconvert", None)

    # 强制使用 NV12（所有树莓派摄像头 100% 支持）
    caps = Gst.Caps.from_string(
        "video/x-raw,format=NV12,width=1280,height=720,framerate=30/1"
    )

    # Tee 分流
    tee = Gst.ElementFactory.make("tee", "t")
    queue_webrtc = Gst.ElementFactory.make("queue")
    queue_record = Gst.ElementFactory.make("queue")

    # WebRTC 分支
    x264_webrtc = Gst.ElementFactory.make("x264enc")
    x264_webrtc.set_property("tune", "zerolatency")
    x264_webrtc.set_property("speed-preset", "veryfast")

    parse_webrtc = Gst.ElementFactory.make("h264parse")
    pay = Gst.ElementFactory.make("rtph264pay")
    pay.set_property("config-interval", 1)
    pay.set_property("pt", 96)

    # webrtcbin
    webrtcbin = Gst.ElementFactory.make("webrtcbin", "sendonly")
    webrtcbin.set_property("bundle-policy", 0)

    # 录制分支
    x264_record = Gst.ElementFactory.make("x264enc")
    x264_record.set_property("tune", "zerolatency")
    x264_record.set_property("speed-preset", "veryfast")

    parse_record = Gst.ElementFactory.make("h264parse")
    mux = Gst.ElementFactory.make("matroskamux")
    sink = Gst.ElementFactory.make("filesink")
    sink.set_property("location", "output_record.mkv")

    # Audio
    audiosrc = Gst.ElementFactory.make("autoaudiosrc")
    aconv = Gst.ElementFactory.make("audioconvert")
    ares = Gst.ElementFactory.make("audioresample")
    aenc = Gst.ElementFactory.make("avenc_aac")
    aparse = Gst.ElementFactory.make("aacparse")
    aqueue = Gst.ElementFactory.make("queue")

    # Add all elements
    for e in [
        src, convert, tee,
        queue_webrtc, x264_webrtc, parse_webrtc, pay, webrtcbin,
        queue_record, x264_record, parse_record, mux, sink,
        audiosrc, aconv, ares, aenc, aparse, aqueue
    ]:
        pipeline.add(e)

    # Link camera → convert
    src.link_filtered(convert, caps)
    convert.link(tee)

    # WebRTC branch
    tee.link(queue_webrtc)
    queue_webrtc.link(x264_webrtc)
    x264_webrtc.link(parse_webrtc)
    parse_webrtc.link(pay)

    pay.connect("pad-added", on_rtph264pay_pad_added, webrtcbin)

    # Recording branch
    tee.link(queue_record)
    queue_record.link(x264_record)
    x264_record.link(parse_record)
    parse_record.link(mux)

    # Audio for mux
    audiosrc.link(aconv)
    aconv.link(ares)
    ares.link(aenc)
    aenc.link(aparse)
    aparse.link(aqueue)
    aqueue.link(mux)

    # mux → file
    mux.link(sink)

    # WebRTC callbacks
    webrtcbin.connect("on-negotiation-needed", on_negotiation_needed)

    return pipeline


# === Main ===
if __name__ == "__main__":
    pipeline = build_pipeline()
    pipeline.set_state(Gst.State.PLAYING)

    print("\nPipeline started.\nWaiting for WebRTC SDP OFFER...\n")

    loop = GLib.MainLoop()
    loop.run()

    pipeline.set_state(Gst.State.NULL)
