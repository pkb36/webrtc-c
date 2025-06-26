gst-launch-1.0 udpsrc port=5002 caps="application/x-rtp, media=(string)video,encoding-name=(string)VP8,payload=96" ! rtpvp8depay ! vp8dec ! autovideosink 
