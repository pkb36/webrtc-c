#!/bin/sh

# Usage: ./convert_video_after_delay.sh <delay_time_in_seconds> <input_webm_filename>

# Check if the correct number of arguments are passed
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <delay_time_in_seconds> <input_webm_filename>"
    exit 1
fi

# Get the delay time and input file from the arguments
DELAY_TIME=$1
INPUT_FILE=$2

# Extract the base filename without extension
BASENAME=$(echo "$INPUT_FILE" | sed 's/\(.*\)\.webm/\1/')

# Print information
echo "Delay time: $DELAY_TIME seconds"
echo "Input file: $INPUT_FILE"
echo "Base name: $BASENAME"

# Delay for the specified time
echo "Waiting for $DELAY_TIME seconds before starting the conversion..."
sleep "$DELAY_TIME"

# Check if BASENAME contains "CAM0" or "CAM1"
if echo "$BASENAME" | grep -q "CAM0"; then
    # If BASENAME contains "CAM0", use this ffmpeg command
    echo "Converting with CAM0 settings..."
	ffmpeg -i "$INPUT_FILE" -c:v libx264 -preset ultrafast -c:a aac -threads 8 -s 640x360 "${BASENAME}.mp4"
elif echo "$BASENAME" | grep -q "CAM1"; then
    # If BASENAME contains "CAM1", use this ffmpeg command
    echo "Converting with CAM1 settings..."
	ffmpeg -i "$INPUT_FILE" -c:v libx264 -preset ultrafast -c:a aac -threads 8 -s 640x360 "${BASENAME}.mp4"	
else
    echo "File name error..."
fi

# Print completion message
echo "Conversion completed: ${BASENAME}.mp4"

