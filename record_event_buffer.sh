#!/bin/bash
duration=${1}
duration2=`expr $duration \* 1000000000`
port1=${2}
port2=`expr $port1 \+ 1`

echo "Start Evetnt Record Buffer ${port1}:${port2} ${duration2}"

gst-launch-1.0 -e -v udpsrc port=${port1} !  queue max-size-buffers=0 max-size-time=0 max-size-bytes=0 min-threshold-time=${duration2} ! udpsink host=127.0.0.1  port=5200 \
    udpsrc port=${port2} !  queue max-size-buffers=0 max-size-time=0 max-size-bytes=0 min-threshold-time=${duration2} ! udpsink host=127.0.0.1  port=5201

