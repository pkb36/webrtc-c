#!/bin/bash
FILE=./libnvdsgst_dspostproc.so
if [ -f $FILE ]; then
    echo nvidia | sudo -S cp $FILE /opt/nvidia/deepstream/deepstream-6.2/lib/gst-plugins
    echo nvidia | sudo -S chmod 755 /opt/nvidia/deepstream/deepstream-6.2/lib/gst-plugins/$FILE
    if [ -f /opt/nvidia/deepstream/deepstream-6.2/lib/gst-plugins/$FILE ]; then
        rm $FILE
        echo "$FILE was installed and removed."
    fi
fi
