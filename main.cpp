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
#include <queue>

// 全局变量
std::atomic<bool> running(true);
cv::Mat current_frame;
std::mutex frame_mutex;
std::string video_dir_global;
pid_t audio_pid = -1;

// 合成任务队列
struct MergeTask {
    std::string video_file;
    std::string audio_file;
    std::string output_file;
};

std::queue<MergeTask> merge_queue;
std::mutex merge_queue_mutex;
std::atomic<bool> is_merging(false);

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

// 异步合成线程
void merge_worker_thread() {
    while (running) {
        MergeTask task;
        bool has_task = false;
        
        {
            std::lock_guard<std::mutex> lock(merge_queue_mutex);
            if (!merge_queue.empty()) {
                task = merge_queue.front();
                merge_queue.pop();
                has_task = true;
                is_merging = true;
            }
        }
        
        if (has_task) {
            std::cout << "\n→ [异步合成] 开始合并: " << task.output_file << std::endl;
            
            // 构建ffmpeg命令
            std::string ffmpeg_cmd = "ffmpeg -y -i " + task.video_file + 
                                    " -i " + task.audio_file + 
                                    " -c:v copy -c:a aac -strict experimental " + 
                                    task.output_file + " > /dev/null 2>&1";
            
            int result = std::system(ffmpeg_cmd.c_str());
            
            if (result == 0 && access(task.output_file.c_str(), F_OK) == 0) {
                std::cout << "✓ [异步合成] 合并成功: " << task.output_file << std::endl;
                
                // 删除原始文件
                remove(task.video_file.c_str());
                remove(task.audio_file.c_str());
                std::cout << "✓ [异步合成] 清理临时文件完成" << std::endl;
            } else {
                std::cerr << "✗ [异步合成] 合并失败，保留原始文件" << std::endl;
            }
            
            is_merging = false;
        } else {
            sleep(2);
        }
    }
}

// 检查并添加未合成的文件到队列
void check_unmerged_files(const std::string& dir) {
    std::cout << "\n→ 检查未合成的视频文件..." << std::endl;
    
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    
    std::vector<std::string> avi_files;
    struct dirent* entry;
    
    while ((entry = readdir(d)) != nullptr) {
        if (entry->d_type == DT_REG) {
            std::string filename = entry->d_name;
            if (filename.length() > 4 && filename.substr(filename.length() - 4) == ".avi") {
                avi_files.push_back(filename);
            }
        }
    }
    closedir(d);
    
    int found_count = 0;
    for (const auto& avi_file : avi_files) {
        std::string base_name = avi_file.substr(0, avi_file.length() - 4);
        std::string wav_file = base_name + ".wav";
        std::string mp4_file = base_name + ".mp4";
        
        std::string avi_path = dir + "/" + avi_file;
        std::string wav_path = dir + "/" + wav_file;
        std::string mp4_path = dir + "/" + mp4_file;
        
        // 检查是否存在对应的wav文件，且不存在mp4文件
        if (access(wav_path.c_str(), F_OK) == 0 && access(mp4_path.c_str(), F_OK) != 0) {
            MergeTask task;
            task.video_file = avi_path;
            task.audio_file = wav_path;
            task.output_file = mp4_path;
            
            std::lock_guard<std::mutex> lock(merge_queue_mutex);
            merge_queue.push(task);
            found_count++;
            
            std::cout << "  + 发现未合成文件: " << avi_file << std::endl;
        }
    }
    
    if (found_count > 0) {
        std::cout << "✓ 找到 " << found_count << " 个未合成文件，已加入队列" << std::endl;
    } else {
        std::cout << "✓ 没有发现未合成的文件" << std::endl;
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

void send_file_download(int client_sock, const std::string& filepath, const std::string& filename) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        std::string error_response = "HTTP/1.1 404 Not Found\r\nContent-Length: 14\r\n\r\nFile not found";
        send(client_sock, error_response.c_str(), error_response.length(), 0);
        return;
    }
    
    // 获取文件大小
    file.seekg(0, std::ios::end);
    long long file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    // 发送响应头
    std::ostringstream header;
    header << "HTTP/1.1 200 OK\r\n";
    header << "Content-Type: application/octet-stream\r\n";
    header << "Content-Disposition: attachment; filename=\"" << filename << "\"\r\n";
    header << "Content-Length: " << file_size << "\r\n";
    header << "Connection: close\r\n\r\n";
    
    std::string header_str = header.str();
    send(client_sock, header_str.c_str(), header_str.length(), 0);
    
    // 发送文件内容
    char buffer[8192];
    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        send(client_sock, buffer, file.gcount(), 0);
    }
    
    file.close();
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
                      << "<td><span class='file-name-link' onclick='downloadFile(\"" << file.name << "\")'>" 
                      << file.name << "</span></td>"
                      << "<td>" << format_size(file.size) << "</td>"
                      << "<td>" << format_time(file.mtime) << "</td>"
                      << "<td>"
                      << "<button class='download-btn' onclick='downloadFile(\"" << file.name << "\")'>下载</button> "
                      << "<button class='delete-btn' onclick='deleteFile(\"" << file.name << "\")'>删除</button>"
                      << "</td>"
                      << "</tr>";
        }
        
        html = replace_placeholder(html, "{{USAGE_PERCENT}}", std::to_string((int)usage_percent));
        html = replace_placeholder(html, "{{FREE_SPACE}}", format_size(free_space));
        html = replace_placeholder(html, "{{TOTAL_SPACE}}", format_size(total_space));
        html = replace_placeholder(html, "{{FILE_ROWS}}", file_rows.str());
        
        send_http_response(client_sock, "text/html; charset=utf-8", html);
    }
    else if (request.find("GET /download?") == 0) {
        size_t pos = request.find("file=");
        if (pos != std::string::npos) {
            size_t end = request.find(" ", pos);
            std::string filename = request.substr(pos + 5, end - pos - 5);
            filename = url_decode(filename);
            
            if (filename.find("/") == std::string::npos && filename.find("..") == std::string::npos) {
                std::string filepath = video_dir_global + "/" + filename;
                send_file_download(client_sock, filepath, filename);
            }
        }
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
    std::cout << "========================================" << std::endl;
    std::cout << "  树莓派音频+视频录制监控系统" << std::endl;
    std::cout << "========================================" << std::endl;
    
    const char* home_dir = std::getenv("HOME");
    if (!home_dir) return -1;

    video_dir_global = std::string(home_dir) + "/videos/";
    std::system(("mkdir -p " + video_dir_global).c_str());
    
    // 启动异步合成线程
    std::thread merge_thread(merge_worker_thread);
    
    // 检查未合成的文件
    check_unmerged_files(video_dir_global);
    
    if (access("/tmp/camfifo", F_OK) == -1) {
        std::system("mkfifo /tmp/camfifo");
    }
    
    // 启动摄像头（MJPEG for Web preview）
    std::cout << "→ 启动摄像头..." << std::endl;
    pid_t cam_pid = fork();
    if (cam_pid == 0) {
        execlp("rpicam-vid", "rpicam-vid", "-t", "0",
               "--codec", "mjpeg", "--width", "640", "--height", "480",
               "--framerate", "20", "-o", "/tmp/camfifo", (char*)NULL);
        exit(1);
    }
    
    std::thread web_thread(web_server_thread);
    
    sleep(2);
    cv::VideoCapture cap("/tmp/camfifo");
    if (!cap.isOpened()) {
        std::cerr << "✗ 无法打开摄像头！" << std::endl;
        running = false;
        kill(cam_pid, SIGTERM);
        web_thread.join();
        merge_thread.join();
        return -1;
    }
    
    std::cout << "✓ 系统就绪！" << std::endl;
    std::cout << "========================================" << std::endl;
    
    int fps = 20;
    int width = 640;
    int height = 480;
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
            audio_filename = video_dir_global + std::string(buf) + ".wav";
            
            writer.open(video_filename, cv::VideoWriter::fourcc('M','J','P','G'), 
                       fps, cv::Size(width, height));
            
            if (!writer.isOpened()) {
                std::cerr << "✗ 无法创建视频文件！" << std::endl;
                sleep(5);
                continue;
            }
            
            // 启动音频录制
            audio_pid = fork();
            if (audio_pid == 0) {
                execlp("arecord", "arecord",
                       "-D", "plughw:CARD=Device,DEV=0",
                       "-f", "cd",
                       "-t", "wav",
                       "-d", "3600",
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
            
            std::cout << "✓ 录制完成，准备异步合成..." << std::endl;
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
                
                // 添加到异步合成队列
                MergeTask task;
                task.video_file = video_filename;
                task.audio_file = audio_filename;
                task.output_file = mp4_filename;
                
                {
                    std::lock_guard<std::mutex> lock(merge_queue_mutex);
                    merge_queue.push(task);
                }
                
                std::cout << "✓ 已添加到合成队列: " << mp4_filename << std::endl;
            } else {
                std::cerr << "✗ 音频或视频文件缺失，无法合成" << std::endl;
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
    merge_thread.join();
    std::cout << "✓ 系统已关闭" << std::endl;
    
    return 0;
}