#include <opencv2/opencv.hpp>
#include <iostream>
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

// 全局变量用于Web流
std::atomic<bool> running(true);
cv::Mat current_frame;
std::mutex frame_mutex;

// 获取磁盘空间，返回剩余空间
long long get_free_disk_space(const std::string& path) {
    struct statvfs buf;
    if (statvfs(path.c_str(), &buf) != 0) {
        std::cerr << "获取磁盘空间失败！" << std::endl;
        return -1;
    }
    return buf.f_bsize * buf.f_bfree;
}

// 删除最旧的文件
void delete_oldest_file(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) {
        std::cerr << "无法打开目录：" << dir << std::endl;
        return;
    }

    struct dirent* entry;
    std::string oldest_file;
    time_t oldest_time = time(nullptr);

    while ((entry = readdir(d)) != nullptr) {
        if (entry->d_type == DT_REG) {
            std::string file_path = dir + "/" + entry->d_name;
            struct stat file_stat;
            if (stat(file_path.c_str(), &file_stat) == 0) {
                if (file_stat.st_mtime < oldest_time) {
                    oldest_time = file_stat.st_mtime;
                    oldest_file = file_path;
                }
            }
        }
    }

    closedir(d);

    if (!oldest_file.empty()) {
        if (remove(oldest_file.c_str()) == 0) {
            std::cout << "删除了最旧的文件：" << oldest_file << std::endl;
        } else {
            std::cerr << "删除文件失败：" << oldest_file << std::endl;
        }
    }
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
        // 返回HTML页面
        std::string html = R"(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>实时视频监控</title>
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            display: flex;
            flex-direction: column;
            align-items: center;
            padding: 20px;
        }
        .container {
            max-width: 1200px;
            width: 100%;
        }
        h1 {
            color: white;
            text-align: center;
            margin-bottom: 30px;
            font-size: 2.5em;
            text-shadow: 2px 2px 4px rgba(0,0,0,0.3);
        }
        .video-container {
            background: white;
            border-radius: 15px;
            padding: 20px;
            box-shadow: 0 10px 40px rgba(0,0,0,0.3);
            margin-bottom: 20px;
        }
        .video-wrapper {
            position: relative;
            width: 100%;
            padding-bottom: 75%; /* 4:3 aspect ratio */
            background: #000;
            border-radius: 10px;
            overflow: hidden;
        }
        #videoStream {
            position: absolute;
            top: 0;
            left: 0;
            width: 100%;
            height: 100%;
            object-fit: contain;
        }
        .info-panel {
            background: white;
            border-radius: 15px;
            padding: 20px;
            box-shadow: 0 10px 40px rgba(0,0,0,0.3);
        }
        .info-item {
            display: flex;
            justify-content: space-between;
            padding: 10px 0;
            border-bottom: 1px solid #eee;
        }
        .info-item:last-child {
            border-bottom: none;
        }
        .info-label {
            font-weight: bold;
            color: #667eea;
        }
        .info-value {
            color: #555;
        }
        .status-indicator {
            display: inline-block;
            width: 10px;
            height: 10px;
            border-radius: 50%;
            background: #4caf50;
            margin-right: 5px;
            animation: pulse 2s infinite;
        }
        @keyframes pulse {
            0%, 100% { opacity: 1; }
            50% { opacity: 0.5; }
        }
        @media (max-width: 768px) {
            h1 {
                font-size: 1.8em;
            }
            .video-container, .info-panel {
                padding: 15px;
            }
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>🎥 实时视频监控系统</h1>
        
        <div class="video-container">
            <div class="video-wrapper">
                <img id="videoStream" src="/stream" alt="视频流">
            </div>
        </div>
        
        <div class="info-panel">
            <div class="info-item">
                <span class="info-label">
                    <span class="status-indicator"></span>状态
                </span>
                <span class="info-value">正在录制</span>
            </div>
            <div class="info-item">
                <span class="info-label">分辨率</span>
                <span class="info-value">640 × 480</span>
            </div>
            <div class="info-item">
                <span class="info-label">帧率</span>
                <span class="info-value">20 FPS</span>
            </div>
            <div class="info-item">
                <span class="info-label">录制时长</span>
                <span class="info-value">60分钟/段</span>
            </div>
        </div>
    </div>
    
    <script>
        // 错误处理
        const img = document.getElementById('videoStream');
        img.onerror = function() {
            console.error('视频流加载失败');
            setTimeout(() => {
                img.src = '/stream?' + new Date().getTime();
            }, 3000);
        };
    </script>
</body>
</html>)";
        
        send_http_response(client_sock, "text/html; charset=utf-8", html);
    }
    else if (request.find("GET /stream") == 0) {
        // 返回MJPEG流
        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n";
        response << "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n";
        response << "Connection: close\r\n";
        response << "\r\n";
        
        std::string resp_str = response.str();
        send(client_sock, resp_str.c_str(), resp_str.length(), 0);
        
        // 持续发送帧
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
            
            // 编码为JPEG
            std::vector<uchar> buf;
            std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 80};
            cv::imencode(".jpg", frame, buf, params);
            
            // 发送MJPEG帧
            std::ostringstream frame_header;
            frame_header << "--frame\r\n";
            frame_header << "Content-Type: image/jpeg\r\n";
            frame_header << "Content-Length: " << buf.size() << "\r\n";
            frame_header << "\r\n";
            
            std::string header_str = frame_header.str();
            if (send(client_sock, header_str.c_str(), header_str.length(), 0) <= 0) break;
            if (send(client_sock, buf.data(), buf.size(), 0) <= 0) break;
            if (send(client_sock, "\r\n", 2, 0) <= 0) break;
            
            usleep(50000); // 50ms延迟
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
    
    std::cout << "Web服务器已启动在 http://0.0.0.0:6969" << std::endl;
    
    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_sock < 0) continue;
        
        // 使用新线程处理每个客户端
        std::thread(handle_client, client_sock).detach();
    }
    
    close(server_sock);
}

int main() {
    // 保证管道文件存在
    if (access("/tmp/camfifo", F_OK) == -1) {
        std::system("mkfifo /tmp/camfifo");
    }
    
    // 启动rpicam-vid后台推流
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
    
    // 启动Web服务器线程
    std::thread web_thread(web_server_thread);
    
    // 主进程稍等等待管道流有效
    usleep(100000);

    // OpenCV读取mjpeg管道流
    cv::VideoCapture cap("/tmp/camfifo");
    if (!cap.isOpened()) {
        std::cerr << "无法打开 rpicam-vid 管道流！" << std::endl;
        running = false;
        kill(pid, SIGTERM);
        web_thread.join();
        return -1;
    }
    
    int fps = 20;
    int width = 640;
    int height = 480;
    int record_duration_sec = 60 * 60;
    auto start_time = std::chrono::steady_clock::now();
    cv::VideoWriter writer;
    std::string filename;

    const char* home_dir = std::getenv("HOME");
    if (!home_dir) {
        std::cerr << "无法获取用户主目录！" << std::endl;
        running = false;
        kill(pid, SIGTERM);
        web_thread.join();
        return -1;
    }

    std::string output_dir = std::string(home_dir) + "/videos/";

    if (access(output_dir.c_str(), F_OK) == -1) {
        std::system(("mkdir -p " + output_dir).c_str());
    }

    while (running) {
        long long free_space = get_free_disk_space(output_dir);
        if (free_space >= 0 && free_space < 1024 * 1024 * 1024) {
            delete_oldest_file(output_dir);
        }

        if (!writer.isOpened()) {
            std::time_t t = std::time(nullptr);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", std::localtime(&t));
            filename = output_dir + std::string(buf) + ".avi";
            
            writer.open(filename, cv::VideoWriter::fourcc('M','J','P','G'), fps, cv::Size(width, height));
            if (!writer.isOpened()) {
                std::cerr << "无法创建视频文件：" << filename << std::endl;
                break;
            }
            start_time = std::chrono::steady_clock::now();
        }
        
        cv::Mat frame;
        if (!cap.read(frame) || frame.empty()) {
            std::cerr << "帧读取失败!" << std::endl;
            exit(1);  // 使用 exit(1) 来表示程序异常终止
            // break;
        }
        
        // 添加时间戳（背景透明，文字半透明）
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
        
        // 创建半透明文字（alpha=0.7，即70%不透明度）
        cv::Mat overlay = frame.clone();
        cv::putText(overlay, time_text, textOrg, fontFace, fontScale, cv::Scalar(255,255,255), thickness);
        cv::addWeighted(overlay, 0.7, frame, 0.3, 0, frame);
        
        // 更新当前帧供Web流使用
        {
            std::lock_guard<std::mutex> lock(frame_mutex);
            current_frame = frame.clone();
        }
        
        writer.write(frame);
        
        if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time).count() >= record_duration_sec) {
            writer.release();
        }
        
        if (cv::waitKey(1) == 'q') {
            running = false;
            break;
        }
    }

    running = false;
    cap.release();
    writer.release();
    cv::destroyAllWindows();
    kill(pid, SIGTERM);
    web_thread.join();
    
    return 0;
}