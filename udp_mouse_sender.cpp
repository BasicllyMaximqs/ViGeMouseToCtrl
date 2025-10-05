#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <fcntl.h> 
#include <unistd.h> // linux lib
#include <sys/socket.h> //linux lib
#include <arpa/inet.h> // linux lib
#include <linux/input.h> // linux lib
#include <libevdev/libevdev.h> // sudo apt install libevdev-dev
 
constexpr int UDP_SERVER_PORT = 12345;

struct MousePayload {
    int deltaX;
    int deltaY;
    bool mLMB;
    bool mRMB;
};


std::atomic<bool> running(true);

std::string findMouseDevice() {
    for (int i = 0; i < 32; ++i) {
        std::string path = "/dev/input/event" + std::to_string(i);
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) continue;

        libevdev *dev = nullptr;
        if (libevdev_new_from_fd(fd, &dev) == 0) {
            if (libevdev_has_event_type(dev, EV_REL)) {
                std::cout << "Found mouse device: " << libevdev_get_name(dev) << " (" << path << ")\n";
                libevdev_free(dev);
                close(fd);
                return path;
            }
            libevdev_free(dev);
        }
        close(fd);
    }
    return "";
}

void InputThread(const std::string& device_path, int udp_sock, sockaddr_in server_addr) {
    int fd = open(device_path.c_str(), O_RDONLY | O_NONBLOCK | O_EXCL);
    if (fd < 0) {
        std::cerr << "Failed to open " << device_path << " (sudo required for O_EXCL)\n";
        running = false;
        return;
    }

    libevdev *dev = nullptr;
    if (libevdev_new_from_fd(fd, &dev) < 0) {
        std::cerr << "Failed to init libevdev\n";
        close(fd);
        running = false;
        return;
    }

    int deltaX = 0;
    int deltaY = 0;
    bool lmb = false;
    bool rmb = false;

    while (running) {
        input_event ev;
        int rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
            if (ev.type == EV_REL) {
                if (ev.code == REL_X) deltaX += ev.value;
                else if (ev.code == REL_Y) deltaY += ev.value;
            } else if (ev.type == EV_KEY) {
                if (ev.code == BTN_LEFT) lmb = (ev.value != 0);
                else if (ev.code == BTN_RIGHT) rmb = (ev.value != 0);
                else if (ev.code == KEY_F2 && ev.value == 1) running = false;
            }
        } else if (rc != -EAGAIN) {
            std::cerr << "Error reading evdev: " << "no placeholder bcus im lazy" << "\n";
            running = false;
            break;
        }

        if (deltaX != 0 || deltaY != 0) {
            MousePayload payload{deltaX, deltaY, lmb, rmb};
            sendto(udp_sock, &payload, sizeof(payload), 0,
                   (struct sockaddr*)&server_addr, sizeof(server_addr));
            deltaX = 0;
            deltaY = 0;
        }
    }

    libevdev_free(dev);
    close(fd);
}

int main() {
    std::cout << "--- Linux UDP Raw Input Sender (High Precision) ---\n";

    std::string mouse_device = findMouseDevice();
    if (mouse_device.empty()) {
        std::cerr << "Could not find mouse device\n";
        return 1;
    }

    std::string server_ip;
    std::cout << "Enter target server IP: ";
    std::cin >> server_ip;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(UDP_SERVER_PORT);
    if (inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr) <= 0) {
        std::cerr << "Invalid IP\n";
        return 1;
    }

    std::thread input_t(InputThread, mouse_device, sock, server_addr);

    std::cout << "Running. Press F2 to exit.\n";
    while (running) {
        //idk just idle the main thread
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    input_t.join();
    close(sock);
    std::cout << "Exiting cleanly.\n";
    return 0;
}
