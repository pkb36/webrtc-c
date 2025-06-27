#include <opencv2/opencv.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/imgproc.hpp>
#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <string>
#include <jsoncpp/json/json.h>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <thread>
#include <chrono>
#include <filesystem>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <signal.h>

#define NUM_CLASSES 6

namespace fs = std::filesystem;

static const std::map<std::string, cv::Scalar> colorMap = {
    {"red", cv::Scalar(0, 0, 255)},
    {"green", cv::Scalar(0, 255, 0)},
    {"blue", cv::Scalar(255, 0, 0)},
    {"yellow", cv::Scalar(0, 255, 255)},
    {"null", cv::Scalar(0, 255, 0)}
};

static const char* class_names[] = {
    "normal_cow",          // CLASS_NORMAL_COW = 0
    "heat_cow",            // CLASS_HEAT_COW = 1
    "flip_cow",            // CLASS_FLIP_COW = 2
    "labor_sign_cow",      // CLASS_LABOR_SIGN_COW = 3
    "normal_cow_sitting",  // CLASS_NORMAL_COW_SITTING = 4
    "over_temp"            // CLASS_OVER_TEMP = 5
};

struct DetectionData {
    std::string timestamp;
    std::string class_name;
    float confidence;
    std::vector<float> bbox;
    std::string bbox_color;
    bool has_bbox;
};

struct ProcessingTask {
    fs::path video_path;
    fs::path json_path;
    fs::path output_path;
};

class AutoOSDMonitor {
private:
    std::string base_path;
    std::queue<ProcessingTask> task_queue;
    std::mutex queue_mutex;
    std::condition_variable cv;
    bool running = true;
    bool processing = false;
    std::thread monitor_thread;
    std::thread processor_thread;
    std::set<std::string> processed_files;
    std::mutex processed_mutex;

public:
    AutoOSDMonitor(const std::string& path = "/home/nvidia/data") : base_path(path) {
        std::cout << "=== 자동 OSD 처리 시스템 시작 ===" << std::endl;
        std::cout << "모니터링 경로: " << base_path << std::endl;
    }

    ~AutoOSDMonitor() {
        stop();
    }

    void start() {
        // 모니터링 스레드 시작
        monitor_thread = std::thread(&AutoOSDMonitor::monitorDirectory, this);
        
        // 처리 스레드 시작
        processor_thread = std::thread(&AutoOSDMonitor::processQueue, this);
        
        std::cout << "모니터링 시작됨. Ctrl+C로 종료하세요." << std::endl;
    }

    void stop() {
        running = false;
        cv.notify_all();
        
        if (monitor_thread.joinable()) {
            monitor_thread.join();
        }
        if (processor_thread.joinable()) {
            processor_thread.join();
        }
        
        std::cout << "\n시스템 종료됨." << std::endl;
    }

    bool isRunning() const {
        return running;
    }

private:
    void createCompletionSignal(const fs::path& video_path, const std::string& camera_id, 
                                int event_class) {
        try {
            // 완료 신호 파일 경로 (.osd_complete)
            fs::path signal_path = video_path;
            signal_path.replace_extension(".osd_complete");
            
            // JSON 형식으로 완료 정보 저장
            Json::Value signal_data;
            signal_data["video_path"] = video_path.string();
            signal_data["camera_id"] = camera_id;
            signal_data["event_class"] = event_class;
            signal_data["completion_time"] = static_cast<Json::Value::Int64>(std::time(nullptr));
            signal_data["snapshot_path"] = video_path.string() + "_snapshot.jpg";
            
            Json::StreamWriterBuilder builder;
            std::ofstream signal_file(signal_path);
            if (signal_file.is_open()) {
                std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
                writer->write(signal_data, &signal_file);
                signal_file.close();
                
                std::cout << "완료 신호 생성: " << signal_path << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "완료 신호 생성 실패: " << e.what() << std::endl;
        }
    }
    
    // 비디오에서 스냅샷 추출
    void extractSnapshot(const fs::path& video_path, int frame_number = 150) {
        try {
            cv::VideoCapture cap(video_path.string());
            if (!cap.isOpened()) return;
            
            // 중간 지점 프레임으로 이동 (15초 지점, 10fps 기준)
            cap.set(cv::CAP_PROP_POS_FRAMES, frame_number);
            
            cv::Mat frame;
            if (cap.read(frame)) {
                std::string snapshot_path = video_path.string() + "_snapshot.jpg";
                cv::imwrite(snapshot_path, frame);
                std::cout << "스냅샷 저장: " << snapshot_path << std::endl;
            }
            cap.release();
        } catch (const std::exception& e) {
            std::cerr << "스냅샷 추출 실패: " << e.what() << std::endl;
        }
    }
    
    // 파일명에서 카메라 ID 추출
    std::string extractCameraId(const std::string& filename) {
        // 예: event_RGB_Camera_20241211_123456.mp4 -> RGB_Camera
        size_t start = filename.find("event_") + 6;
        size_t end = filename.find("_2024");  // 년도 부분 찾기
        if (start != std::string::npos && end != std::string::npos) {
            return filename.substr(start, end - start);
        }
        return "unknown";
    }

    void monitorDirectory() {
        while (running) {
            try {
                // EVENT_YYYYMMDD 패턴의 디렉토리 찾기
                for (const auto& entry : fs::directory_iterator(base_path)) {
                    if (!running) break;
                    
                    if (entry.is_directory()) {
                        std::string dir_name = entry.path().filename().string();
                        if (dir_name.find("EVENT_") == 0 && dir_name.length() == 14) {
                            scanEventDirectory(entry.path());
                        }
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "디렉토리 모니터링 오류: " << e.what() << std::endl;
            }
            
            // 5초마다 스캔
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }

    void scanEventDirectory(const fs::path& event_dir) {
        // .detections.json 파일 찾기
        for (const auto& entry : fs::directory_iterator(event_dir)) {
            if (!running) break;
            
            if (entry.path().extension() == ".json" && 
                entry.path().string().find(".detections.json") != std::string::npos) {
                
                std::string json_file = entry.path().string();
                
                // 이미 처리한 파일인지 확인
                {
                    std::lock_guard<std::mutex> lock(processed_mutex);
                    if (processed_files.find(json_file) != processed_files.end()) {
                        continue;
                    }
                }
                
                // 대응하는 MP4 파일 찾기
                std::string base_name = json_file.substr(0, json_file.find(".detections.json"));
                std::string video_file = base_name + ".mp4";
                
                if (fs::exists(video_file)) {
                    // 파일이 완전히 쓰여졌는지 확인 (크기가 변하지 않을 때까지 대기)
                    if (isFileReady(video_file) && isFileReady(json_file)) {
                        ProcessingTask task;
                        task.json_path = entry.path();
                        task.video_path = video_file;
                        task.output_path = base_name + "_temp.mp4";
                        
                        {
                            std::lock_guard<std::mutex> lock(queue_mutex);
                            task_queue.push(task);
                            
                            std::lock_guard<std::mutex> lock2(processed_mutex);
                            processed_files.insert(json_file);
                        }
                        
                        cv.notify_one();
                        
                        std::cout << "\n새 작업 발견: " << entry.path().filename() << std::endl;
                    }
                }
            }
        }
    }

    bool isFileReady(const std::string& filepath) {
        try {
            auto size1 = fs::file_size(filepath);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto size2 = fs::file_size(filepath);
            return size1 == size2 && size1 > 0;
        } catch (...) {
            return false;
        }
    }

    void processQueue() {
        while (running) {
            ProcessingTask task;
            
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                cv.wait(lock, [this] { return !task_queue.empty() || !running; });
                
                if (!running) break;
                
                if (!task_queue.empty()) {
                    task = task_queue.front();
                    task_queue.pop();
                    processing = true;
                }
            }
            
            if (processing) {
                processVideo(task);
                processing = false;
            }
        }
    }

    void processVideo(const ProcessingTask& task) {
        std::cout << "\n처리 시작: " << task.video_path.filename() << " (원본 파일 대체)" << std::endl;
        auto start_time = std::chrono::high_resolution_clock::now();
        
        try {
            // JSON 로드
            std::string camera_id = "unknown";
            int event_class = 0;

            std::map<int, std::vector<DetectionData>> frame_detections;
            // JSON 파일 읽기
            std::ifstream json_file(task.json_path);
            if (!json_file.is_open()) {
                std::cerr << "JSON 파일 열기 실패: " << task.json_path << std::endl;
                return;
            }
            
            Json::Value root;
            json_file >> root;
            json_file.close();

            std::cout << "JSON 내용 : " << root.toStyledString() << std::endl;

            // event_class 값 추출
            if (root.isMember("event_class")) {
                event_class = root["event_class"].asInt();
                std::cout << "이벤트 클래스: " << event_class << std::endl;
            }
            
            // 검출 데이터 로드
            if (!loadDetectionData(task.json_path.string(), frame_detections)) {
                std::cerr << "검출 데이터 로드 실패" << std::endl;
                return;
            }

            if(root.isMember("camera"))
            {
                camera_id = root["camera"].asString();
                std::cout << "카메라 ID: " << camera_id << std::endl;
            } else {
                std::cerr << "카메라 ID 정보가 없습니다." << std::endl;
                return;
            }

            // 비디오 처리
            if (processVideoFile(task.video_path.string(), task.output_path.string(), frame_detections)) {
                auto end_time = std::chrono::high_resolution_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time).count();
                
                std::cout << "처리 완료: " << task.output_path.filename() 
                        << " (" << duration << "초)" << std::endl;
                
                // 원본 파일을 임시 파일로 교체
                try {
                    fs::remove(task.video_path);
                    fs::rename(task.output_path, task.video_path);
                    std::cout << "원본 파일 교체 완료: " << task.video_path.filename() << std::endl;
                    
                    // 스냅샷 추출
                    extractSnapshot(task.video_path);
                    
                    // 완료 신호 생성 (event_class 포함)
                    createCompletionSignal(task.video_path, camera_id, event_class);
                    
                } catch (const std::exception& e) {
                    std::cerr << "파일 교체 실패: " << e.what() << std::endl;
                    return;
                }
                
                // JSON 파일 삭제
                try {
                    fs::remove(task.json_path);
                    std::cout << "JSON 파일 삭제됨: " << task.json_path.filename() << std::endl;
                } catch (const std::exception& e) {
                    std::cerr << "JSON 파일 삭제 실패: " << e.what() << std::endl;
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "처리 중 오류: " << e.what() << std::endl;
        }
    }

    bool loadDetectionData(const std::string& json_file, 
                      std::map<int, std::vector<DetectionData>>& frame_detections) {
        std::ifstream file(json_file);
        if (!file.is_open()) {
            return false;
        }

        Json::Value root;
        file >> root;

        std::string event_time = root["event_time"].asString();
        auto event_timestamp = parseTimestamp(event_time);
        auto video_start_time = event_timestamp - 15.0;

        std::cout << "이벤트 타임스탬프: " << event_time 
                << ", 절대 시간: " << event_timestamp 
                << ", 시작 시간: " << video_start_time << std::endl;

        const Json::Value& detections = root["detections"];
        for (const auto& detection : detections) {
            DetectionData det;
            
            // 타임스탬프 처리 - 나노초 정수인 경우
            if (detection["timestamp"].isString()) {
                det.timestamp = detection["timestamp"].asString();
            } else if (detection["timestamp"].isNumeric()) {
                // 나노초를 초로 변환
                int64_t nano_timestamp = detection["timestamp"].asInt64();
                double timestamp_seconds = nano_timestamp / 1000000000.0;
                det.timestamp = std::to_string(timestamp_seconds);
            }
            
            int class_id = detection["class_id"].asInt();
            if (class_id >= 0 && class_id < NUM_CLASSES) {
                det.class_name = class_names[class_id];
            } else {
                det.class_name = "unknown";
            }
            
            det.confidence = detection["confidence"].asFloat();

            const Json::Value& bbox = detection["bbox"];
            for (const auto& coord : bbox) {
                det.bbox.push_back(coord.asFloat());
            }

            det.bbox_color = detection["bbox_color"].asString();
            det.has_bbox = detection["has_bbox"].asBool();

            // 타임스탬프를 double로 변환
            double det_timestamp;
            if (detection["timestamp"].isNumeric()) {
                int64_t nano_timestamp = detection["timestamp"].asInt64();
                det_timestamp = nano_timestamp / 1000000000.0;
            } else {
                det_timestamp = parseTimestamp(det.timestamp);
            }
            
            double relative_time = det_timestamp - video_start_time;
            
            std::cout << "검출 상대 시간: " << relative_time << "초" << std::endl;

            if (relative_time >= 0 && relative_time <= 30) {
                int frame_num = static_cast<int>(relative_time * 10.0); // 10 FPS
                frame_detections[frame_num].push_back(det);
            }
        }

        return true;
    }

    bool processVideoFile(const std::string& input_video, const std::string& output_video,
                         const std::map<int, std::vector<DetectionData>>& frame_detections) {
        // 하드웨어 디코더 시도
        cv::VideoCapture cap;
        std::string gst_pipeline = "filesrc location=" + input_video + " ! "
                                  "qtdemux ! h264parse ! nvv4l2decoder ! "
                                  "nvvidconv ! video/x-raw,format=BGRx ! "
                                  "videoconvert ! video/x-raw,format=BGR ! "
                                  "appsink";
        
        cap.open(gst_pipeline, cv::CAP_GSTREAMER);
        
        if (!cap.isOpened()) {
            cap.open(input_video);
        }

        if (!cap.isOpened()) {
            std::cerr << "비디오 파일 열기 실패: " << input_video << std::endl;
            return false;
        }

        double fps = cap.get(cv::CAP_PROP_FPS);
        int width = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
        int height = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
        int total_frames = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_COUNT));

        // 품질 2 고정 - MP4V 코덱
        cv::VideoWriter writer;
        int fourcc = cv::VideoWriter::fourcc('M', 'P', '4', 'V');
        
        // 하드웨어 인코더 시도
        std::string gst_writer = "appsrc ! "
                                "video/x-raw,format=BGR,width=" + std::to_string(width) + 
                                ",height=" + std::to_string(height) + 
                                ",framerate=" + std::to_string((int)fps) + "/1 ! "
                                "videoconvert ! video/x-raw,format=BGRx ! "
                                "nvvidconv ! nvv4l2h264enc bitrate=4000000 ! "
                                "h264parse ! qtmux ! "
                                "filesink location=" + output_video;
        
        writer.open(gst_writer, cv::CAP_GSTREAMER, 0, fps, cv::Size(width, height));
        
        if (!writer.isOpened()) {
            writer.open(output_video, fourcc, fps, cv::Size(width, height));
        }

        if (!writer.isOpened()) {
            std::cerr << "비디오 작성기 열기 실패" << std::endl;
            return false;
        }

        cv::Mat frame;
        int frame_count = 0;

        while (cap.read(frame)) {
            // 검출 데이터 그리기
            auto it = frame_detections.find(frame_count);

            if (it != frame_detections.end()) {
                drawDetections(frame, it->second);
            }

            writer.write(frame);
            frame_count++;

            // 진행률 표시 (10% 단위)
            if (frame_count % (total_frames / 10 + 1) == 0) {
                std::cout << "." << std::flush;
            }
        }
        std::cout << std::endl;

        cap.release();
        writer.release();

        return true;
    }

    void drawDetections(cv::Mat& frame, const std::vector<DetectionData>& detections) {
        std::cout << "프레임에 그릴 검출 데이터 수: " << detections.size() << std::endl;
        
        for (const auto& det : detections) {
            if (det.bbox.size() < 4) continue;

            cv::Scalar color = cv::Scalar(0, 255, 0); // 기본 색상

            auto it = colorMap.find(det.bbox_color);
            if (it != colorMap.end()) {
                color = it->second;
            }

            // 정규화된 좌표를 픽셀 좌표로 변환
            int x1 = det.bbox[0] * frame.cols;
            int y1 = det.bbox[1] * frame.rows;
            int x2 = det.bbox[2] * frame.cols;
            int y2 = det.bbox[3] * frame.rows;

            // 바운딩 박스
            cv::rectangle(frame,
                        cv::Point(x1, y1),
                        cv::Point(x2, y2),
                        color, 3);
            
            std::cout << "바운딩 박스: [" 
                    << x1 << ", " << y1 << ", "
                    << x2 << ", " << y2 << "] (원본: ["
                    << det.bbox[0] << ", " << det.bbox[1] << ", "
                    << det.bbox[2] << ", " << det.bbox[3] << "])"
                    << ", 색상: " << det.bbox_color 
                    << std::endl;

            // 텍스트
            std::string label = det.class_name + " " + 
                            std::to_string(static_cast<int>(det.confidence * 100)) + "%";

            int text_y = std::max(y1 - 5, 20);
            
            // 텍스트 배경
            int baseline = 0;
            cv::Size text_size = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, 
                                            0.7, 2, &baseline);
            
            cv::rectangle(frame,
                        cv::Point(x1, text_y - text_size.height - 5),
                        cv::Point(x1 + text_size.width, text_y + 5),
                        cv::Scalar(0, 0, 0), cv::FILLED);

            // 텍스트
            cv::putText(frame, label,
                    cv::Point(x1, text_y),
                    cv::FONT_HERSHEY_SIMPLEX, 0.7,
                    cv::Scalar(255, 255, 255), 2);
        }
    }

    double parseTimestamp(const std::string& timestamp) {
        struct tm tm = {};
        std::istringstream ss(timestamp);
        ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
        
        double seconds = mktime(&tm);
        
        size_t dot_pos = timestamp.find('.');
        if (dot_pos != std::string::npos) {
            std::string subsec = timestamp.substr(dot_pos);
            double subseconds = std::stod(subsec);
            seconds += subseconds;
        }
        
        return seconds;
    }
};

// 전역 인스턴스
AutoOSDMonitor* g_monitor = nullptr;

// 시그널 핸들러
void signalHandler(int signum) {
    std::cout << "\n종료 신호 받음..." << std::endl;
    if (g_monitor) {
        g_monitor->stop();
    }
}

int main(int argc, char* argv[]) {
    // 시그널 핸들러 설정
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    std::string monitor_path = "/home/nvidia/data";
    if (argc > 1) {
        monitor_path = argv[1];
    }

    // 모니터 생성 및 시작
    g_monitor = new AutoOSDMonitor(monitor_path);
    g_monitor->start();

    // 메인 스레드는 대기
    while (g_monitor->isRunning()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    delete g_monitor;

    return 0;
}