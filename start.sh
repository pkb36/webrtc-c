echo " === Start WebRTC script ====  "
unset DISPLAY
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/home/nvidia/webrtc

cd /home/nvidia/webrtc
sleep 5
echo " === Start WebRTC server ====  "
echo nvidia | sudo -S python /home/nvidia/webrtc/gstream_manage.py &

echo " === Start theremal camera connection check  ==== "
echo nvidia | sudo -S python /home/nvidia/webrtc/thermal_check.py &

if [ -d /home/nvidia/rest_server_bin ]; then
    echo " === Start Rest server ====  "
    export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/home/nvidia/rest_server_bin
    python /home/nvidia/rest_server_bin/rest_manage.py &
fi
