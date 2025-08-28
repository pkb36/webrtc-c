# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a WebRTC streaming system designed for NVIDIA Jetson platforms, specifically for real-time video streaming from RGB and thermal cameras with AI object detection capabilities. The system consists of approximately 14,261 lines of C code and 1,087 lines of header files.

### Core Architecture

The system follows a multi-process architecture:
- **gstream_main**: Parent process that handles signaling server communication and spawns child processes
- **webrtc_sender**: Child processes (one per client) that handle individual WebRTC connections
- **Python utilities**: Camera management and disk utilities

### Key Components

1. **WebRTC Streaming**: Real-time video streaming using GStreamer WebRTC implementation
2. **AI Inference**: NVIDIA DeepStream integration with YOLOv7 for object detection
3. **Multi-camera Support**: Handles both RGB (1920x1080) and thermal (384x288) cameras
4. **PTZ Control**: Pan-tilt-zoom camera control via serial communication
5. **Event Management**: Motion detection with recording and notification capabilities

## Build Commands

### Main Build System
```bash
# Build all targets (recommended)
make

# Build individual components
make build/gstream_main      # Main streaming application
make build/webrtc_sender     # WebRTC client handler

# Install to current directory
make install

# Install to specific location
make install-to DEST_DIR=/path/to/install

# Clean build artifacts
make clean
```

### Python Utilities
```bash
# In python/ directory
make                    # Build event OSD maker
make run               # Run with default data path
make daemon            # Run as background service
make status            # Check if running
make stop              # Stop background process
```

### Testing Commands
```bash
# Individual test programs (optional builds)
make build/json_test     # JSON utilities test
make build/settting_test # Device settings test
make build/log_test      # Logging system test
```

## Dependencies

### System Requirements
- NVIDIA Jetson platform with DeepStream 6.2
- GStreamer with WebRTC support
- libsoup-2.4 for WebSocket communication
- json-glib-1.0 for JSON parsing
- libcurl for HTTP operations

### Hardware Integration
- Serial communication for PTZ control (typically /dev/ttyTHS0 at 38400 baud)
- UDP video sources (ports 8877 for RGB, 8878 for thermal)
- Network configuration for WebRTC (base ports 5000+ for streams, 6000+ for signaling)

## Configuration

### Primary Config Files
- `config.json`: Main system configuration including camera settings, network ports, server endpoints
- `device_setting.json`: Device-specific settings for cameras and AI models
- `RGB_yoloV7.txt` / `Thermal_yoloV7.txt`: AI model configuration files

### Key Configuration Parameters
- **camera_id**: Unique device identifier
- **stream_base_port**: Starting port for UDP streams (default: 5000)
- **comm_socket_port**: Base port for inter-process communication (default: 6000)
- **max_stream_cnt**: Maximum concurrent client connections (default: 10)
- **server_ip**: WebSocket signaling server endpoint

## Development Workflow

### Code Organization
- **Core streaming**: `gstream_main.c`, `webrtc_sender.c`, `webrtc_peer.c`
- **Communication**: `socket_comm.c`, `serial_comm.c`, `curllib.c`
- **AI processing**: `nvds_process.c`, `nvds_utils.c`
- **Configuration**: `config.c`, `device_setting.c`
- **Utilities**: `json_utils.c`, logging system (`log.c`, `g_log.c`, `log_wrapper.c`)

### Feature Flags in global_define.h
- `TRACK_PERSON_INCLUDE`: Enable person tracking
- `OPTICAL_FLOW_INCLUDE`: Enable motion analysis
- `THERMAL_TEMP_INCLUDE`: Enable thermal temperature processing
- `RESNET_50`: Use ResNet-50 for inference

### Multi-Process Communication
The system uses Unix domain sockets for inter-process communication between gstream_main and webrtc_sender processes. Each client connection spawns a dedicated webrtc_sender process with allocated ports.

### AI Pipeline Integration
- DeepStream pipelines configured via text files (RGB_yoloV7.txt, Thermal_yoloV7.txt)
- Object detection with configurable bounding box colors and confidence thresholds
- Real-time inference results integrated into video streams via NvDsOSD

### Thread Safety
Multiple pthread mutexes ensure thread-safe operation:
- `g_send_mutex`: Server communication
- `g_process_msg_mutex`: Message processing
- `g_send_info_mutex`: Information sharing
- Thread-safe queue implementation for event handling

This system is specifically designed for production deployment on NVIDIA Jetson platforms in surveillance and monitoring applications requiring real-time AI-enhanced video streaming capabilities.