#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <hiredis/hiredis.h>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

volatile sig_atomic_t stopFlag = 0;

void onSignal(int) {
    stopFlag = 1;
}

bool writeAll(int fd, const char* buf, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, buf + written, len - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += static_cast<size_t>(n);
    }
    return true;
}

redisContext* connectRedis(const std::string& host, int port) {
    while (!stopFlag) {
        redisContext* c = redisConnect(host.c_str(), port);
        if (c && !c->err) {
            return c;
        }
        if (c) {
            redisFree(c);
        }
        sleep(1);
    }
    return nullptr;
}

bool processFile(const std::string& name, std::string& summary, const std::string& workerId) {
    std::string inPath = "/data/in/" + name;
    std::string outPath = "/data/out/" + name;

    int inFd = open(inPath.c_str(), O_RDONLY);
    if (inFd < 0) {
        return false;
    }
    struct stat st;
    if (fstat(inFd, &st) != 0) {
        close(inFd);
        return false;
    }

    int outFd = open(outPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (outFd < 0) {
        close(inFd);
        return false;
    }

    size_t size = static_cast<size_t>(st.st_size);
    size_t lines = 0;
    uint64_t hash = 14695981039346656037ULL;
    bool ok = true;

    if (size > 0) {
        void* mapped = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, inFd, 0);
        if (mapped == MAP_FAILED) {
            close(outFd);
            close(inFd);
            return false;
        }
        const unsigned char* data = static_cast<const unsigned char*>(mapped);
        std::vector<char> outBuf;
        outBuf.reserve(65536);

        for (size_t i = 0; i < size; ++i) {
            unsigned char c = data[i];
            if (c == '\n') {
                ++lines;
            }
            hash ^= c;
            hash *= 1099511628211ULL;
            outBuf.push_back(static_cast<char>(std::toupper(c)));
            if (outBuf.size() == 65536) {
                if (!writeAll(outFd, outBuf.data(), outBuf.size())) {
                    ok = false;
                    break;
                }
                outBuf.clear();
            }
        }
        if (ok && !outBuf.empty()) {
            ok = writeAll(outFd, outBuf.data(), outBuf.size());
        }
        munmap(mapped, size);
    }

    close(outFd);
    close(inFd);

    std::ostringstream oss;
    oss << "worker=" << workerId
        << " file=" << name
        << " bytes=" << size
        << " lines=" << lines
        << " hash=" << std::hex << std::setw(16) << std::setfill('0') << hash;
    summary = oss.str();
    return ok;
}

int main() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    char hostBuf[256];
    std::memset(hostBuf, 0, sizeof(hostBuf));
    std::string workerId = (gethostname(hostBuf, sizeof(hostBuf) - 1) == 0) ? hostBuf : "worker";

    const char* hostEnv = std::getenv("REDIS_HOST");
    std::string host = hostEnv ? hostEnv : "broker";
    int port = 6379;

    mkdir("/data/in", 0755);
    mkdir("/data/out", 0755);

    redisContext* redis = connectRedis(host, port);
    if (!redis) {
        return 0;
    }

    std::cout << "worker " << workerId << " ready" << std::endl;

    while (!stopFlag) {
        redisReply* task = static_cast<redisReply*>(redisCommand(redis, "BLPOP tasks 1"));
        if (!task) {
            if (stopFlag) {
                break;
            }
            redisFree(redis);
            redis = connectRedis(host, port);
            if (!redis) {
                break;
            }
            continue;
        }
        if (task->type == REDIS_REPLY_ARRAY && task->elements == 2) {
            std::string name = task->element[1]->str;
            freeReplyObject(task);

            std::string summary;
            if (processFile(name, summary, workerId)) {
                redisReply* push = static_cast<redisReply*>(
                    redisCommand(redis, "RPUSH results %s", summary.c_str())
                );
                if (push) {
                    freeReplyObject(push);
                }
                std::cout << summary << std::endl;
            }
        } else {
            freeReplyObject(task);
        }
    }

    if (redis) {
        redisFree(redis);
    }
    std::cout << "worker " << workerId << " stopped" << std::endl;
    return 0;
}