#!/bin/bash
gst-launch-1.0 videotestsrc pattern=smpte is-live=True ! clockoverlay time-format=\"%D %H:%M:%S\" ! video/x-raw,width=1280,height=720,framerate=15/1 ! videoconvert ! vp8enc keyframe-max-dist=30 target-bitrate=15000 ! rtpvp8pay ! udpsink host=127.0.0.1  port=5000

