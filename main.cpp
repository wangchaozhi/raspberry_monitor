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
        if (entry->d_type == DT_REG) {  // 只处理文件
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

int main() {
    // 保证管道文件存在
    if (access("/tmp/camfifo", F_OK) == -1) {
        std::system("mkfifo /tmp/camfifo");
    }
    
    // 启动rpicam-vid后台推流
    pid_t pid = fork();
    if (pid == 0) { // 子进程
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
    
    // 主进程稍等等待管道流有效
    usleep(600000);

    // OpenCV读取mjpeg管道流
    cv::VideoCapture cap("/tmp/camfifo");
    if (!cap.isOpened()) {
        std::cerr << "无法打开 rpicam-vid 管道流！" << std::endl;
        kill(pid, SIGTERM);
        return -1;
    }
    
    int fps = 20;
    int width = 640;
    int height = 480;
    int record_duration_sec = 60 * 60; // 60分钟
    auto start_time = std::chrono::steady_clock::now();
    cv::VideoWriter writer;
    std::string filename;

    // 获取当前用户的主目录
    const char* home_dir = std::getenv("HOME");
    if (!home_dir) {
        std::cerr << "无法获取用户主目录！" << std::endl;
        return -1;
    }

    // 设置视频文件保存目录为用户主目录下的 videos 文件夹
    std::string output_dir = std::string(home_dir) + "/videos/";

    // 检查目标目录是否存在，不存在则创建
    if (access(output_dir.c_str(), F_OK) == -1) {
        // 目录不存在，创建目录
        std::system(("mkdir -p " + output_dir).c_str());
    }

    while (true) {
        // 检查磁盘空间，剩余空间小于1GB时删除最旧的视频文件
        long long free_space = get_free_disk_space(output_dir);
        if (free_space >= 0 && free_space < 1024 * 1024 * 1024) {
            delete_oldest_file(output_dir);  // 删除最旧的视频文件
        }

        if (!writer.isOpened()) {
            // 获取当前时间并格式化为文件名
            std::time_t t = std::time(nullptr);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", std::localtime(&t));
            filename = output_dir + std::string(buf) + ".avi";  // 将目录和文件名合并
            
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
        cv::rectangle(frame, textOrg + cv::Point(0, baseline), textOrg + cv::Point(textSize.width, -textSize.height), cv::Scalar(0,0,0), cv::FILLED);
        cv::putText(frame, time_text, textOrg, fontFace, fontScale, cv::Scalar(255,255,255), thickness);
        
        writer.write(frame);
        
        if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time).count() >= record_duration_sec) {
            writer.release();
        }
        if (cv::waitKey(1) == 'q') break;
    }

    cap.release();
    writer.release();
    cv::destroyAllWindows();
    // 关闭后台rpicam-vid
    kill(pid, SIGTERM);
    return 0;
}
