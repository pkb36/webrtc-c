#!/bin/bash

duration=${1}
duration2=`expr $duration \* 60000000000`

dir=`date +_%y_%m_%d_%H_%M_%S`
record_dir="/home/nvidia/data/RECORD${dir}"
if [ ! -d $record_dir ]; then
    mkdir $record_dir
fi


port1=${2}
cam_name1=${3}
full_dir1="${record_dir}/${cam_name1}_%07d.webm"
echo "Start Record ${port1} ${duration2} ${full_dir1}"
gst-launch-1.0 udpsrc port=${port1} ! "application/x-rtp, media=(string)video, clock-rate=(int)90000, encoding-name=(string)VP9, payload=(int)96" ! rtpvp9depay ! queue ! splitmuxsink location=${full_dir1} max-size-time=${duration2} muxer=webmmux &


port2=${4}
cam_name2=${5}
full_dir2="${record_dir}/${cam_name2}_%07d.webm"
echo "Start Record ${port2} ${duration2} ${full_dir2}"
gst-launch-1.0 udpsrc port=${port2} ! "application/x-rtp, media=(string)video, clock-rate=(int)90000, encoding-name=(string)VP9, payload=(int)96" ! rtpvp9depay ! queue ! splitmuxsink location=${full_dir2} max-size-time=${duration2} muxer=webmmux &

