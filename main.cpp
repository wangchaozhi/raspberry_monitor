#include <opencv2/opencv.hpp>
#include <iostream>
#include <fstream>
#include <ctime>
#include <sstream>
#include <string>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <chrono>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <sys/statvfs.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <vector>
#include <algorithm>

// 全局变量
std::atomic<bool> running(true);
cv::Mat current_frame;
std::mutex frame_mutex;
std::string video_dir_global;
pid_t audio_pid = -1;

// ==================== 配置区域 ====================

// 分辨率预设枚举
enum class Resolution {
    SD_480P,      // 640×480   - 标清，省空间
    HD_720P,      // 1280×720  - 高清，推荐
    FHD_1080P,    // 1920×1080 - 全高清
    ULTRA_5MP     // 2592×1944 - 超高清（USB摄像头最大）
};

// 帧率预设枚举
enum class FrameRate {
    FPS_15,       // 15 FPS - 省空间
    FPS_20,       // 20 FPS - 平衡
    FPS_25,       // 25 FPS - PAL标准
    FPS_30        // 30 FPS - 流畅
};

// 音频质量预设枚举
enum class AudioQuality {
    LOW,          // 64kbps  - 省空间，语音清晰
    MEDIUM,       // 96kbps  - 平衡
    HIGH,         // 128kbps - 高质量
    VERY_HIGH     // 192kbps - 音乐级
};

// ========== 在这里选择配置 ==========
const bool USE_USB_CAMERA = true;           // true=USB摄像头, false=CSI摄像头
const int USB_CAMERA_INDEX = 1;             // /dev/video1

const Resolution RECORDING_RESOLUTION = Resolution::HD_720P;   // 选择分辨率
const FrameRate RECORDING_FRAMERATE = FrameRate::FPS_20;      // 选择帧率
const AudioQuality AUDIO_QUALITY = AudioQuality::MEDIUM;      // 选择音频质量
// ====================================

// 分辨率映射
struct ResolutionConfig {
    int width;
    int height;
    const char* name;
};

ResolutionConfig getResolution(Resolution res) {
    switch (res) {
        case Resolution::SD_480P:
            return {640, 480, "640×480 (标清)"};
        case Resolution::HD_720P:
            return {1280, 720, "1280×720 (高清)"};
        case Resolution::FHD_1080P:
            return {1920, 1080, "1920×1080 (全高清)"};
        case Resolution::ULTRA_5MP:
            return {2592, 1944, "2592×1944 (5MP超清)"};
        default:
            return {1280, 720, "1280×720 (高清)"};
    }
}

// 帧率映射
struct FrameRateConfig {
    int fps;
    const char* name;
};

FrameRateConfig getFrameRate(FrameRate fr) {
    switch (fr) {
        case FrameRate::FPS_15:
            return {15, "15 FPS (省空间)"};
        case FrameRate::FPS_20:
            return {20, "20 FPS (平衡)"};
        case FrameRate::FPS_25:
            return {25, "25 FPS (PAL)"};
        case FrameRate::FPS_30:
            return {30, "30 FPS (流畅)"};
        default:
            return {20, "20 FPS (平衡)"};
    }
}

// 音频质量映射
struct AudioQualityConfig {
    int bitrate_kbps;
    int sample_rate;
    const char* name;
    const char* bitrate_str;
};

AudioQualityConfig getAudioQuality(AudioQuality aq) {
    switch (aq) {
        case AudioQuality::LOW:
            return {64, 22050, "低质量 (语音)", "64k"};
        case AudioQuality::MEDIUM:
            return {96, 22050, "中质量 (推荐)", "96k"};
        case AudioQuality::HIGH:
            return {128, 44100, "高质量", "128k"};
        case AudioQuality::VERY_HIGH:
            return {192, 44100, "超高质量 (音乐)", "192k"};
        default:
            return {96, 22050, "中质量 (推荐)", "96k"};
    }
}

// ==================== 配置区域结束 ====================

// 文件信息结构体
struct FileInfo {
    std::string name;
    std::string path;
    long long size;
    time_t mtime;
    
    bool operator<(const FileInfo& other) const {
        return mtime > other.mtime;
    }
};

long long get_free_disk_space(const std::string& path) {
    struct statvfs buf;
    if (statvfs(path.c_str(), &buf) != 0) return -1;
    return (long long)buf.f_frsize * (long long)buf.f_bavail;
}

long long get_total_disk_space(const std::string& path) {
    struct statvfs buf;
    if (statvfs(path.c_str(), &buf) != 0) return -1;
    return (long long)buf.f_frsize * (long long)buf.f_blocks;
}

std::vector<FileInfo> get_video_files(const std::string& dir) {
    std::vector<FileInfo> files;
    DIR* d = opendir(dir.c_str());
    if (!d) return files;

    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        if (entry->d_type == DT_REG) {
            std::string filename = entry->d_name;
            if ((filename.length() > 4 && filename.substr(filename.length() - 4) == ".avi") ||
                (filename.length() > 4 && filename.substr(filename.length() - 4) == ".wav") ||
                (filename.length() > 4 && filename.substr(filename.length() - 4) == ".aac") ||
                (filename.length() > 4 && filename.substr(filename.length() - 4) == ".mp4")) {
                std::string file_path = dir + "/" + filename;
                struct stat file_stat;
                if (stat(file_path.c_str(), &file_stat) == 0) {
                    FileInfo info;
                    info.name = filename;
                    info.path = file_path;
                    info.size = file_stat.st_size;
                    info.mtime = file_stat.st_mtime;
                    files.push_back(info);
                }
            }
        }
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    return files;
}

std::string get_oldest_file(const std::string& dir) {
    std::vector<FileInfo> files = get_video_files(dir);
    if (files.empty()) return "";
    return files.back().path;
}

std::string format_size(long long bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + " KB";
    if (bytes < 1024 * 1024 * 1024) return std::to_string(bytes / 1024 / 1024) + " MB";
    return std::to_string(bytes / 1024 / 1024 / 1024) + " GB";
}

std::string format_time(time_t t) {
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return std::string(buf);
}

void ensure_disk_space(const std::string& dir, long long min_space_bytes) {
    int deleted_count = 0;
    
    while (deleted_count < 20) {
        long long free_space = get_free_disk_space(dir);
        if (free_space >= min_space_bytes) break;
        
        std::string oldest = get_oldest_file(dir);
        if (oldest.empty()) break;
        
        if (remove(oldest.c_str()) == 0) {
            deleted_count++;
            std::cout << "✓ 删除旧文件: " << oldest << std::endl;
        } else {
            break;
        }
    }
}

std::string url_decode(const std::string& str) {
    std::string result;
    for (size_t i = 0; i < str.length(); i++) {
        if (str[i] == '%' && i + 2 < str.length()) {
            int value;
            std::istringstream is(str.substr(i + 1, 2));
            if (is >> std::hex >> value) {
                result += static_cast<char>(value);
                i += 2;
            } else {
                result += str[i];
            }
        } else if (str[i] == '+') {
            result += ' ';
        } else {
            result += str[i];
        }
    }
    return result;
}

std::string read_file(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) return "";
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string replace_placeholder(const std::string& html, const std::string& placeholder, const std::string& value) {
    std::string result = html;
    size_t pos = 0;
    while ((pos = result.find(placeholder, pos)) != std::string::npos) {
        result.replace(pos, placeholder.length(), value);
        pos += value.length();
    }
    return result;
}

void send_http_response(int client_sock, const std::string& content_type, const std::string& body) {
    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n";
    response << "Content-Type: " << content_type << "\r\n";
    response << "Content-Length: " << body.length() << "\r\n";
    response << "Connection: close\r\n\r\n" << body;
    
    std::string resp_str = response.str();
    send(client_sock, resp_str.c_str(), resp_str.length(), 0);
}

void handle_client(int client_sock) {
    char buffer[4096];
    int bytes_read = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
    
    if (bytes_read <= 0) {
        close(client_sock);
        return;
    }
    
    buffer[bytes_read] = '\0';
    std::string request(buffer);
    
    if (request.find("GET / ") == 0 || request.find("GET /index.html") == 0) {
        std::string html = read_file("index.html");
        if (html.empty()) {
            send_http_response(client_sock, "text/plain", "Error: index.html not found!");
            close(client_sock);
            return;
        }
        
        long long free_space = get_free_disk_space(video_dir_global);
        long long total_space = get_total_disk_space(video_dir_global);
        long long used_space = total_space - free_space;
        double usage_percent = (double)used_space / total_space * 100.0;
        
        std::vector<FileInfo> files = get_video_files(video_dir_global);
        
        std::ostringstream file_rows;
        for (const auto& file : files) {
            file_rows << "<tr>"
                      << "<td>" << file.name << "</td>"
                      << "<td>" << format_size(file.size) << "</td>"
                      << "<td>" << format_time(file.mtime) << "</td>"
                      << "<td><button class='delete-btn' onclick='deleteFile(\"" << file.name << "\")'>删除</button></td>"
                      << "</tr>";
        }
        
        html = replace_placeholder(html, "{{USAGE_PERCENT}}", std::to_string((int)usage_percent));
        html = replace_placeholder(html, "{{FREE_SPACE}}", format_size(free_space));
        html = replace_placeholder(html, "{{TOTAL_SPACE}}", format_size(total_space));
        html = replace_placeholder(html, "{{FILE_ROWS}}", file_rows.str());
        
        send_http_response(client_sock, "text/html; charset=utf-8", html);
    }
    else if (request.find("GET /delete?") == 0) {
        size_t pos = request.find("file=");
        if (pos != std::string::npos) {
            size_t end = request.find(" ", pos);
            std::string filename = request.substr(pos + 5, end - pos - 5);
            filename = url_decode(filename);
            
            if (filename.find("/") == std::string::npos && filename.find("..") == std::string::npos) {
                std::string filepath = video_dir_global + "/" + filename;
                if (remove(filepath.c_str()) == 0) {
                    send_http_response(client_sock, "text/plain; charset=utf-8", "删除成功！");
                } else {
                    send_http_response(client_sock, "text/plain; charset=utf-8", "删除失败");
                }
            }
        }
    }
    else if (request.find("GET /stream") == 0) {
        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n";
        response << "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n";
        response << "Connection: close\r\n\r\n";
        
        std::string resp_str = response.str();
        send(client_sock, resp_str.c_str(), resp_str.length(), 0);
        
        while (running) {
            cv::Mat frame;
            {
                std::lock_guard<std::mutex> lock(frame_mutex);
                if (current_frame.empty()) {
                    usleep(50000);
                    continue;
                }
                frame = current_frame.clone();
            }
            
            std::vector<uchar> buf;
            cv::imencode(".jpg", frame, buf, {cv::IMWRITE_JPEG_QUALITY, 80});
            
            std::ostringstream frame_header;
            frame_header << "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " 
                        << buf.size() << "\r\n\r\n";
            
            std::string header_str = frame_header.str();
            if (send(client_sock, header_str.c_str(), header_str.length(), 0) <= 0) break;
            if (send(client_sock, buf.data(), buf.size(), 0) <= 0) break;
            if (send(client_sock, "\r\n", 2, 0) <= 0) break;
            
            usleep(50000);
        }
    }
    
    close(client_sock);
}

void web_server_thread() {
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) return;
    
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(6969);
    
    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(server_sock);
        return;
    }
    
    listen(server_sock, 5);
    std::cout << "✓ Web服务器: http://0.0.0.0:6969" << std::endl;
    
    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
        if (client_sock >= 0) {
            std::thread(handle_client, client_sock).detach();
        }
    }
    close(server_sock);
}

int main() {
    // 获取配置
    ResolutionConfig resConfig = getResolution(RECORDING_RESOLUTION);
    FrameRateConfig fpsConfig = getFrameRate(RECORDING_FRAMERATE);
    AudioQualityConfig audioConfig = getAudioQuality(AUDIO_QUALITY);
    
    std::cout << "========================================" << std::endl;
    std::cout << "  树莓派音频+视频录制监控系统" << std::endl;
    if (USE_USB_CAMERA) {
        std::cout << "  摄像头: USB Camera (/dev/video" << USB_CAMERA_INDEX << ")" << std::endl;
    } else {
        std::cout << "  摄像头: CSI Camera (rpicam-vid)" << std::endl;
    }
    std::cout << "  分辨率: " << resConfig.name << std::endl;
    std::cout << "  帧率: " << fpsConfig.name << std::endl;
    std::cout << "  音频: " << audioConfig.name << " (" << audioConfig.bitrate_kbps << "kbps)" << std::endl;
    std::cout << "========================================" << std::endl;
    
    const char* home_dir = std::getenv("HOME");
    if (!home_dir) return -1;

    video_dir_global = std::string(home_dir) + "/videos/";
    std::system(("mkdir -p " + video_dir_global).c_str());
    
    std::thread web_thread(web_server_thread);
    
    cv::VideoCapture cap;
    pid_t cam_pid = -1;
    
    if (USE_USB_CAMERA) {
        // 使用USB摄像头（使用V4L2后端避免GStreamer警告）
        std::cout << "→ 打开USB摄像头..." << std::endl;
        cap.open(USB_CAMERA_INDEX, cv::CAP_V4L2);
        
        if (!cap.isOpened()) {
            std::cerr << "✗ 无法打开USB摄像头 /dev/video" << USB_CAMERA_INDEX << std::endl;
            running = false;
            web_thread.join();
            return -1;
        }
        
        // 设置分辨率和帧率
        cap.set(cv::CAP_PROP_FRAME_WIDTH, resConfig.width);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, resConfig.height);
        cap.set(cv::CAP_PROP_FPS, fpsConfig.fps);
        
        // 读取实际值
        int actual_width = (int)cap.get(cv::CAP_PROP_FRAME_WIDTH);
        int actual_height = (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT);
        int actual_fps = (int)cap.get(cv::CAP_PROP_FPS);
        
        std::cout << "✓ USB摄像头已就绪" << std::endl;
        std::cout << "  实际分辨率: " << actual_width << "x" << actual_height << std::endl;
        if (actual_fps > 0) {
            std::cout << "  实际帧率: " << actual_fps << " FPS" << std::endl;
        } else {
            std::cout << "  帧率: 自动" << std::endl;
        }
        
    } else {
        // 使用CSI摄像头（原有方式）
        if (access("/tmp/camfifo", F_OK) == -1) {
            std::system("mkfifo /tmp/camfifo");
        }
        
        std::cout << "→ 启动CSI摄像头..." << std::endl;
        cam_pid = fork();
        if (cam_pid == 0) {
            execlp("rpicam-vid", "rpicam-vid", "-t", "0",
                   "--codec", "mjpeg", "--width", "640", "--height", "480",
                   "--framerate", "20", "-o", "/tmp/camfifo", (char*)NULL);
            exit(1);
        }
        
        sleep(2);
        cap.open("/tmp/camfifo");
        
        if (!cap.isOpened()) {
            std::cerr << "✗ 无法打开CSI摄像头！" << std::endl;
            running = false;
            kill(cam_pid, SIGTERM);
            web_thread.join();
            return -1;
        }
        
        std::cout << "✓ CSI摄像头已就绪" << std::endl;
    }
    
    std::cout << "✓ 系统就绪！" << std::endl;
    std::cout << "========================================" << std::endl;
    
    int fps = fpsConfig.fps;
    int width = resConfig.width;
    int height = resConfig.height;
    int record_duration_sec = 3600;  // 1小时
    
    cv::VideoWriter writer;
    std::string video_filename;
    std::string audio_filename;
    auto start_time = std::chrono::steady_clock::now();

    while (running) {
        // 开始新的录制
        if (!writer.isOpened()) {
            ensure_disk_space(video_dir_global, 500 * 1024 * 1024);
            
            std::time_t t = std::time(nullptr);
            char buf[64];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", std::localtime(&t));
            
            video_filename = video_dir_global + std::string(buf) + ".avi";
            audio_filename = video_dir_global + std::string(buf) + ".aac";  // 直接录制AAC
            
            // 使用H.264编码（更好的压缩）
            // 尝试硬件加速编码器，失败则用软件编码
            int codec = cv::VideoWriter::fourcc('H','2','6','4');
            writer.open(video_filename, codec, fps, cv::Size(width, height));
            
            if (!writer.isOpened()) {
                std::cout << "→ H.264硬件编码失败，尝试软件编码..." << std::endl;
                codec = cv::VideoWriter::fourcc('X','2','6','4');
                writer.open(video_filename, codec, fps, cv::Size(width, height));
            }
            
            if (!writer.isOpened()) {
                std::cout << "→ H.264编码失败，使用MJPEG..." << std::endl;
                codec = cv::VideoWriter::fourcc('M','J','P','G');
                writer.open(video_filename, codec, fps, cv::Size(width, height));
            }
            
            if (!writer.isOpened()) {
                std::cerr << "✗ 无法创建视频文件！" << std::endl;
                sleep(5);
                continue;
            }
            
            // 直接录制为AAC格式（压缩音频）
            audio_pid = fork();
            if (audio_pid == 0) {
                // 使用ffmpeg直接录制为AAC
                std::string sample_rate_str = std::to_string(audioConfig.sample_rate);
                std::string duration_str = "3600";  // 1小时
                
                execlp("ffmpeg", "ffmpeg",
                       "-f", "alsa",
                       "-i", "plughw:CARD=Device,DEV=0",
                       "-ac", "1",                                    // 单声道
                       "-ar", sample_rate_str.c_str(),                // 采样率
                       "-c:a", "aac",                                 // AAC编码
                       "-b:a", audioConfig.bitrate_str,               // 比特率
                       "-t", duration_str.c_str(),                    // 录制时长
                       audio_filename.c_str(),
                       (char*)NULL);
                exit(1);
            }
            
            std::cout << "\n✓ 开始录制:" << std::endl;
            std::cout << "  视频: " << video_filename << std::endl;
            std::cout << "  音频: " << audio_filename << std::endl;
            
            start_time = std::chrono::steady_clock::now();
        }
        
        cv::Mat frame;
        if (!cap.read(frame) || frame.empty()) {
            usleep(10000);
            continue;
        }
        
        // 添加时间戳
        std::time_t t = std::time(nullptr);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        cv::putText(frame, buf, cv::Point(10, frame.rows - 10),
                   cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(255, 255, 255), 2);
        
        {
            std::lock_guard<std::mutex> lock(frame_mutex);
            current_frame = frame.clone();
        }
        
        writer.write(frame);
        
        // 检查是否录制满1小时
        if (std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time).count() >= record_duration_sec) {
            
            std::cout << "✓ 录制完成，准备合并音视频..." << std::endl;
            writer.release();
            
            if (audio_pid > 0) {
                kill(audio_pid, SIGTERM);
                waitpid(audio_pid, NULL, 0);
                audio_pid = -1;
            }
            
            // 等待文件写入完成
            sleep(2);
            
            // 生成MP4文件名
            std::string mp4_filename = video_filename.substr(0, video_filename.length() - 4) + ".mp4";
            
            // 检查音视频文件是否存在
            if (access(video_filename.c_str(), F_OK) == 0 && 
                access(audio_filename.c_str(), F_OK) == 0) {
                
                std::cout << "→ 合并音视频: " << mp4_filename << std::endl;
                
                // 构建ffmpeg命令 - 音频已经是AAC，直接复制
                std::string ffmpeg_cmd = "ffmpeg -y -i " + video_filename + 
                                        " -i " + audio_filename + 
                                        " -c:v libx264 -preset medium -crf 23 " +  // H.264视频压缩
                                        " -c:a copy " +                             // AAC音频直接复制
                                        mp4_filename + " 2>&1 | grep -E '(error|Error|ERROR)' || true";
                
                int result = std::system(ffmpeg_cmd.c_str());
                
                if (result == 0 && access(mp4_filename.c_str(), F_OK) == 0) {
                    std::cout << "✓ 合并成功！" << std::endl;
                    
                    // 删除原始AVI和AAC文件
                    std::cout << "→ 清理临时文件..." << std::endl;
                    remove(video_filename.c_str());
                    remove(audio_filename.c_str());
                    std::cout << "✓ 清理完成" << std::endl;
                } else {
                    std::cerr << "✗ 合并失败，保留原始文件" << std::endl;
                }
            } else {
                std::cerr << "✗ 音频或视频文件缺失，无法合并" << std::endl;
            }
        }
    }

    std::cout << "\n→ 关闭系统..." << std::endl;
    running = false;
    cap.release();
    writer.release();
    
    if (cam_pid > 0) kill(cam_pid, SIGTERM);
    if (audio_pid > 0) kill(audio_pid, SIGTERM);
    
    web_thread.join();
    std::cout << "✓ 系统已关闭" << std::endl;
    
    return 0;
}