import gi
gi.require_version('Gst', '1.0')
gi.require_version('GstVideo', '1.0')
from gi.repository import Gst, GLib, GObject, GstVideo

# GStreamer 초기화
Gst.init(None)

# 프로브 콜백 함수 정의
def probe_callback(pad, info, user_data):
    buffer = info.get_buffer()
    
    # 타임스탬프 정보 출력
    pts = buffer.pts
    dts = buffer.dts
    duration = buffer.duration
    
    print(f"Buffer received - PTS: {pts}, DTS: {dts}, Duration: {duration}")
    
    # 버퍼 메타데이터 분석 (예: 프레임 크기 등)
    caps = pad.get_current_caps()
    if caps:
        structure = caps.get_structure(0)
        width = structure.get_int("width").value
        height = structure.get_int("height").value
        print(f"Frame size: {width}x{height}")
    
    # 특정 처리 후 버퍼를 계속 전달
    return Gst.PadProbeReturn.OK

# 파이프라인 문자열 구성
pipeline_str = (
    "v4l2src device=/dev/video0 ! clockoverlay time-format=\"%D %H:%M:%S\" font-desc=\"Arial, 18\" ! "
    "videoscale ! videorate ! video/x-raw,width=1280,height=720,framerate=15/1 ! queue ! tee name=video_src_tee0 "
    "video_src_tee0. ! queue ! nvvideoconvert ! RGB.sink_0 nvstreammux name=RGB batch-size=1 width=1280 height=720 live-source=1 ! "
    "nvinfer config-file-path=RGB_yoloV7.txt name=nvinfer_1 ! nvinfer config-file-path=config_infer_secondary.txt name=resnet50_1 ! "
    "nvof ! nvvideoconvert ! dspostproc name=dspostproc_1 ! nvdsosd name=nvosd_1 ! nvvideoconvert ! fakesink"
)

# 파이프라인 생성
pipeline = Gst.parse_launch(pipeline_str)

# 원하는 요소의 pad 가져오기 (예: nvinfer_1 요소의 src 패드)
infer_element = pipeline.get_by_name("nvinfer_1")
if infer_element:
    pad = infer_element.get_static_pad("src")
    if pad:
        # 프로브 추가
        pad.add_probe(Gst.PadProbeType.BUFFER, probe_callback, None)
        print("Successfully added probe to nvinfer_1 src pad")
    else:
        print("Failed to get src pad from nvinfer_1")
else:
    print("Failed to find nvinfer_1 element")

# 메인 루프 시작
pipeline.set_state(Gst.State.PLAYING)

# GLib 메인 루프 실행
main_loop = GLib.MainLoop()
try:
    main_loop.run()
except KeyboardInterrupt:
    pass

# 정리
pipeline.set_state(Gst.State.NULL)