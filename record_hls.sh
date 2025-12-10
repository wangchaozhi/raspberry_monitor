#!/bin/bash

# 创建 HLS 输出目录
mkdir -p /home/wangchaozhi/Desktop/raspi/project/cproject/raspberry_monitor/hls_output

rpicam-vid -t 0 \
  --width 1920 --height 1080 --framerate 30 \
  --codec libav --libav-video-codec h264_v4l2m2m --libav-format matroska \
  --libav-audio --audio-source alsa --audio-device "plughw:CARD=Device,DEV=0" \
  -o - | ffmpeg -y -i - \
  -vf "drawtext=fontfile=/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf:text='%{localtime\:%Y-%m-%d %H\\:%M\\:%S}':fontcolor=white@0.8:fontsize=24:x=10:y=H-th-10:box=1:boxcolor=0x000000AA" \
  -c:v h264_v4l2m2m -b:v 4000k -g 60 -bf 0 -tune zerolatency \
  -c:a aac -b:a 128k \
  -f tee "[f=hls:hls_time=10:hls_list_size=3:hls_flags=delete_segments]/home/wangchaozhi/Desktop/raspi/project/cproject/raspberry_monitor/hls_output/stream.m3u8|[f=matroska]/home/wangchaozhi/Desktop/raspi/project/cproject/raspberry_monitor/archive.mkv"
