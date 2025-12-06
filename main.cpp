#include <opencv2/opencv.hpp>
#include <iostream>
#include <fstream>
#include <ctime>
#include <sstream>
#include <string>
#include <unistd.h>
#include <sys/types.h>
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

// 文件信息结构体
struct FileInfo {
    std::string name;
    std::string path;
    long long size;
    time_t mtime;
    
    bool operator<(const FileInfo& other) const {
        return mtime > other.mtime; // 新的在前
    }
};

// 获取磁盘空间，返回剩余空间（字节）
long long get_free_disk_space(const std::string& path) {
    struct statvfs buf;
    if (statvfs(path.c_str(), &buf) != 0) {
        std::cerr << "获取磁盘空间失败！" << std::endl;
        return -1;
    }
    return (long long)buf.f_frsize * (long long)buf.f_bavail;
}

// 获取磁盘总空间
long long get_total_disk_space(const std::string& path) {
    struct statvfs buf;
    if (statvfs(path.c_str(), &buf) != 0) {
        return -1;
    }
    return (long long)buf.f_frsize * (long long)buf.f_blocks;
}

// 获取目录中的所有视频文件
std::vector<FileInfo> get_video_files(const std::string& dir) {
    std::vector<FileInfo> files;
    DIR* d = opendir(dir.c_str());
    if (!d) {
        return files;
    }

    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        if (entry->d_type == DT_REG) {
            std::string filename = entry->d_name;
            if (filename.length() > 4 && filename.substr(filename.length() - 4) == ".avi") {
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

// 获取最旧的文件路径
std::string get_oldest_file(const std::string& dir) {
    std::vector<FileInfo> files = get_video_files(dir);
    if (files.empty()) return "";
    return files.back().path;
}

// 格式化文件大小
std::string format_size(long long bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + " KB";
    if (bytes < 1024 * 1024 * 1024) return std::to_string(bytes / 1024 / 1024) + " MB";
    return std::to_string(bytes / 1024 / 1024 / 1024) + " GB";
}

// 格式化时间
std::string format_time(time_t t) {
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return std::string(buf);
}

// 确保磁盘空间大于指定值
void ensure_disk_space(const std::string& dir, long long min_space_bytes) {
    int max_attempts = 20;
    int deleted_count = 0;
    
    std::cout << "\n→ 检查磁盘空间..." << std::endl;
    
    while (deleted_count < max_attempts) {
        long long free_space = get_free_disk_space(dir);
        long long total_space = get_total_disk_space(dir);
        
        if (free_space < 0 || total_space < 0) {
            std::cerr << "✗ 无法获取磁盘空间信息" << std::endl;
            break;
        }
        
        long long used_space = total_space - free_space;
        double usage_percent = (double)used_space / total_space * 100.0;
        
        std::cout << "  总空间: " << format_size(total_space) 
                  << " | 已用: " << format_size(used_space) 
                  << " (" << (int)usage_percent << "%)"
                  << " | 可用: " << format_size(free_space) << std::endl;
        
        if (free_space >= min_space_bytes) {
            std::cout << "✓ 磁盘空间充足" << std::endl;
            break;
        }
        
        std::cout << "⚠ 磁盘空间不足，需要至少: " << format_size(min_space_bytes) << std::endl;
        
        std::string oldest = get_oldest_file(dir);
        if (oldest.empty()) {
            std::cerr << "✗ 没有可删除的视频文件！" << std::endl;
            break;
        }
        
        struct stat file_stat;
        long long file_size = 0;
        if (stat(oldest.c_str(), &file_stat) == 0) {
            file_size = file_stat.st_size;
        }
        
        std::cout << "→ 删除最旧文件: " << oldest 
                  << " (" << format_size(file_size) << ")" << std::endl;
        
        if (remove(oldest.c_str()) == 0) {
            deleted_count++;
            std::cout << "✓ 删除成功 [" << deleted_count << "]" << std::endl;
            usleep(100000);
        } else {
            std::cerr << "✗ 删除失败: " << strerror(errno) << std::endl;
            break;
        }
    }
    
    if (deleted_count > 0) {
        std::cout << "✓ 共删除 " << deleted_count << " 个旧视频文件" << std::endl;
    }
}

// URL解码
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

// 读取文件内容
std::string read_file(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// 替换字符串中的所有占位符（支持多次出现）
std::string replace_placeholder(const std::string& html, const std::string& placeholder, const std::string& value) {
    std::string result = html;
    size_t pos = 0;
    while ((pos = result.find(placeholder, pos)) != std::string::npos) {
        result.replace(pos, placeholder.length(), value);
        pos += value.length(); // 移动到替换后的位置
    }
    return result;
}

// 发送HTTP响应
void send_http_response(int client_sock, const std::string& content_type, const std::string& body) {
    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n";
    response << "Content-Type: " << content_type << "\r\n";
    response << "Content-Length: " << body.length() << "\r\n";
    response << "Connection: close\r\n";
    response << "\r\n";
    response << body;
    
    std::string resp_str = response.str();
    send(client_sock, resp_str.c_str(), resp_str.length(), 0);
}

// 处理客户端请求
void handle_client(int client_sock) {
    char buffer[4096];
    int bytes_read = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
    
    if (bytes_read <= 0) {
        close(client_sock);
        return;
    }
    
    buffer[bytes_read] = '\0';
    std::string request(buffer);
    
    // 解析请求路径
    if (request.find("GET / ") == 0 || request.find("GET /index.html") == 0) {
        // 读取HTML模板
        std::string html = read_file("index.html");
        if (html.empty()) {
            std::string error_msg = "Error: index.html not found!";
            send_http_response(client_sock, "text/plain", error_msg);
            close(client_sock);
            return;
        }
        
        // 获取磁盘空间信息
        long long free_space = get_free_disk_space(video_dir_global);
        long long total_space = get_total_disk_space(video_dir_global);
        long long used_space = total_space - free_space;
        double usage_percent = (double)used_space / total_space * 100.0;
        
        // 获取视频文件列表
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
        
        // 替换占位符
        html = replace_placeholder(html, "{{USAGE_PERCENT}}", std::to_string((int)usage_percent));
        html = replace_placeholder(html, "{{FREE_SPACE}}", format_size(free_space));
        html = replace_placeholder(html, "{{TOTAL_SPACE}}", format_size(total_space));
        html = replace_placeholder(html, "{{FILE_ROWS}}", file_rows.str());
        
        send_http_response(client_sock, "text/html; charset=utf-8", html);
    }
    else if (request.find("GET /api/disk") == 0) {
        // API端点：返回磁盘信息JSON
        long long free_space = get_free_disk_space(video_dir_global);
        long long total_space = get_total_disk_space(video_dir_global);
        long long used_space = total_space - free_space;
        double usage_percent = (double)used_space / total_space * 100.0;
        
        std::ostringstream json;
        json << "{"
             << "\"free\":\"" << format_size(free_space) << "\","
             << "\"total\":\"" << format_size(total_space) << "\","
             << "\"used\":\"" << format_size(used_space) << "\","
             << "\"percent\":" << (int)usage_percent
             << "}";
        
        send_http_response(client_sock, "application/json", json.str());
    }
    else if (request.find("GET /api/files") == 0) {
        // API端点：返回文件列表JSON
        std::vector<FileInfo> files = get_video_files(video_dir_global);
        
        std::ostringstream json;
        json << "{\"files\":[";
        for (size_t i = 0; i < files.size(); i++) {
            if (i > 0) json << ",";
            json << "{"
                 << "\"name\":\"" << files[i].name << "\","
                 << "\"size\":\"" << format_size(files[i].size) << "\","
                 << "\"time\":\"" << format_time(files[i].mtime) << "\""
                 << "}";
        }
        json << "]}";
        
        send_http_response(client_sock, "application/json", json.str());
    }
    else if (request.find("GET /delete?") == 0) {
        // 解析文件名
        size_t pos = request.find("file=");
        if (pos != std::string::npos) {
            size_t end = request.find(" ", pos);
            std::string filename = request.substr(pos + 5, end - pos - 5);
            filename = url_decode(filename);
            
            // 安全检查
            if (filename.find("/") == std::string::npos && 
                filename.find("..") == std::string::npos &&
                filename.length() > 4 && 
                filename.substr(filename.length() - 4) == ".avi") {
                
                std::string filepath = video_dir_global + "/" + filename;
                
                if (remove(filepath.c_str()) == 0) {
                    send_http_response(client_sock, "text/plain; charset=utf-8", "删除成功！");
                    std::cout << "✓ Web删除文件: " << filepath << std::endl;
                } else {
                    send_http_response(client_sock, "text/plain; charset=utf-8", "删除失败: " + std::string(strerror(errno)));
                    std::cerr << "✗ Web删除失败: " << filepath << std::endl;
                }
            } else {
                send_http_response(client_sock, "text/plain; charset=utf-8", "非法的文件名！");
            }
        }
    }
    else if (request.find("GET /stream") == 0) {
        // MJPEG流
        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n";
        response << "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n";
        response << "Connection: close\r\n";
        response << "\r\n";
        
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
            std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 80};
            cv::imencode(".jpg", frame, buf, params);
            
            std::ostringstream frame_header;
            frame_header << "--frame\r\n";
            frame_header << "Content-Type: image/jpeg\r\n";
            frame_header << "Content-Length: " << buf.size() << "\r\n";
            frame_header << "\r\n";
            
            std::string header_str = frame_header.str();
            if (send(client_sock, header_str.c_str(), header_str.length(), 0) <= 0) break;
            if (send(client_sock, buf.data(), buf.size(), 0) <= 0) break;
            if (send(client_sock, "\r\n", 2, 0) <= 0) break;
            
            usleep(50000);
        }
    }
    
    close(client_sock);
}

// Web服务器线程
void web_server_thread() {
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        std::cerr << "创建socket失败！" << std::endl;
        return;
    }
    
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(6969);
    
    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "绑定端口6969失败！" << std::endl;
        close(server_sock);
        return;
    }
    
    if (listen(server_sock, 5) < 0) {
        std::cerr << "监听失败！" << std::endl;
        close(server_sock);
        return;
    }
    
    std::cout << "✓ Web服务器已启动: http://0.0.0.0:6969" << std::endl;
    
    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_sock < 0) continue;
        
        std::thread(handle_client, client_sock).detach();
    }
    
    close(server_sock);
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  树莓派视频录制与Web监控系统" << std::endl;
    std::cout << "========================================" << std::endl;
    
    if (access("/tmp/camfifo", F_OK) == -1) {
        std::system("mkfifo /tmp/camfifo");
        std::cout << "✓ 创建管道文件: /tmp/camfifo" << std::endl;
    }
    
    std::cout << "→ 启动摄像头推流..." << std::endl;
    pid_t pid = fork();
    if (pid == 0) {
        execlp("rpicam-vid", "rpicam-vid",
               "-t", "0",
               "--codec", "mjpeg",
               "--width", "640",
               "--height", "480",
               "--framerate", "20",
               "-o", "/tmp/camfifo",
               (char*)NULL);
        perror("启动rpicam-vid失败");
        exit(1);
    }
    
    std::thread web_thread(web_server_thread);
    
    std::cout << "→ 等待摄像头初始化..." << std::endl;
    usleep(600000);

    cv::VideoCapture cap("/tmp/camfifo");
    if (!cap.isOpened()) {
        std::cerr << "✗ 无法打开 rpicam-vid 管道流！" << std::endl;
        running = false;
        kill(pid, SIGTERM);
        web_thread.join();
        return -1;
    }
    
    std::cout << "✓ 摄像头管道流已就绪" << std::endl;
    
    int fps = 20;
    int width = 640;
    int height = 480;
    int record_duration_sec = 60 * 60;
    auto start_time = std::chrono::steady_clock::now();
    cv::VideoWriter writer;
    std::string filename;

    const char* home_dir = std::getenv("HOME");
    if (!home_dir) {
        std::cerr << "✗ 无法获取用户主目录！" << std::endl;
        running = false;
        kill(pid, SIGTERM);
        web_thread.join();
        return -1;
    }

    std::string output_dir = std::string(home_dir) + "/videos/";
    video_dir_global = output_dir;

    if (access(output_dir.c_str(), F_OK) == -1) {
        std::system(("mkdir -p " + output_dir).c_str());
        std::cout << "✓ 创建视频目录: " << output_dir << std::endl;
    }
    
    std::cout << "✓ 视频保存目录: " << output_dir << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "系统运行中... 按 Ctrl+C 停止" << std::endl;
    std::cout << "========================================" << std::endl;

    while (running) {
        if (!writer.isOpened()) {
            long long min_space = 200 * 1024 * 1024;
            std::cout << "\n→ 准备创建新视频文件，检查磁盘空间..." << std::endl;
            ensure_disk_space(output_dir, min_space);
            
            std::time_t t = std::time(nullptr);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", std::localtime(&t));
            filename = output_dir + std::string(buf) + ".avi";
            
            writer.open(filename, cv::VideoWriter::fourcc('M','J','P','G'), fps, cv::Size(width, height));
            if (!writer.isOpened()) {
                std::cerr << "✗ 无法创建视频文件：" << filename << std::endl;
                break;
            }
            std::cout << "✓ 开始录制新视频: " << filename << std::endl;
            start_time = std::chrono::steady_clock::now();
        }
        
        cv::Mat frame;
        if (!cap.read(frame) || frame.empty()) {
            std::cerr << "✗ 帧读取失败!" << std::endl;
            break;
        }
        
        std::time_t t = std::time(nullptr);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        std::string time_text(buf);
        int baseline = 0;
        int fontFace = cv::FONT_HERSHEY_SIMPLEX;
        double fontScale = 1.0;
        int thickness = 2;
        cv::Size textSize = cv::getTextSize(time_text, fontFace, fontScale, thickness, &baseline);
        cv::Point textOrg(frame.cols - textSize.width - 10, frame.rows - 10);
        
        cv::Mat overlay = frame.clone();
        cv::putText(overlay, time_text, textOrg, fontFace, fontScale, cv::Scalar(255,255,255), thickness);
        cv::addWeighted(overlay, 0.7, frame, 0.3, 0, frame);
        
        {
            std::lock_guard<std::mutex> lock(frame_mutex);
            current_frame = frame.clone();
        }
        
        writer.write(frame);
        
        if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time).count() >= record_duration_sec) {
            std::cout << "✓ 视频段录制完成: " << filename << std::endl;
            writer.release();
        }
        
        if (cv::waitKey(1) == 'q') {
            running = false;
            break;
        }
    }

    std::cout << "\n→ 正在关闭系统..." << std::endl;
    running = false;
    cap.release();
    writer.release();
    cv::destroyAllWindows();
    kill(pid, SIGTERM);
    
    web_thread.join();
    
    std::cout << "✓ 系统已安全关闭" << std::endl;
    
    return 0;
}