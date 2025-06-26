echo nvidia | sudo -S /home/nvidia/webrtc/disk_check /home/nvidia/data 70 5
rm -f cam0_snapshot.jpg.* cam1_snapshot.jpg.*
if [ -d /home/nvidia/.vscode-server ]; then
	rm -rf /home/nvidia/.vscode-server
fi
