#!/bin/bash
rm -f /home/nvidia/webrtc/gstream_main
cp /home/nvidia/webrtc_service/webrtc/gstream_main .
#rm -f /home/nvidia/webrtc/gstream_manage.py
#cp /home/nvidia/webrtc_service/webrtc/gstream_manage.py .
#cp ../gst-dspostproc/gst-dspostproc/libnvdsgst_dspostproc.so .
#./install_tracker_lib.sh
chmod u+x ./gstream_main
unset DISPLAY

