#!/bin/bash

while read line; do
	TIME=$(date "+%Y-%m-%dT%H:%M:%S")
	DATE=$(date "+%Y-%m-%d")
	FILENAME=$DATE.log
	utput="STANDARD:[$TIME] $line";    
	IRECTORY="/home/nvidia/webrtc/logs"

	if [ ! -d "$DIRECTORY" ]; then
		mkdir $DIRECTORY
		chmod u+w "$DIRECTORY"
		echo "$DIRECTORY was created!"
	fi
	if [ ! -f "$DIRECTORY/$FILENAME" ]; then
		touch "$DIRECTORY/$FILENAME"
		chmod u+w "$DIRECTORY/$FILENAME"
		echo "$DIRECTORY/$FILENAME was created!"
	fi
	echo $output >> $DIRECTORY/$FILENAME
done

