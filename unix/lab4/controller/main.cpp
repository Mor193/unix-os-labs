#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <hiredis/hiredis.h>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

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

bool copyFile(const std::string& src, const std::string& dst) {
    int inFd = open(src.c_str(), O_RDONLY);
    if (inFd < 0) {
        return false;
    }
    int outFd = open(dst.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (outFd < 0) {
        close(inFd);
        return false;
    }
    char buf[65536];
    bool ok = true;
    while (true) {
        ssize_t n = read(inFd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = false;
            break;
        }
        if (n == 0) {
            break;
        }
        if (!writeAll(outFd, buf, static_cast<size_t>(n))) {
            ok = false;
            break;
        }
    }
    close(inFd);
    close(outFd);
    return ok;
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

int main() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    const char* hostEnv = std::getenv("REDIS_HOST");
    std::string host = hostEnv ? hostEnv : "broker";
    int port = 6379;

    mkdir("/data/in", 0755);
    mkdir("/data/out", 0755);

    redisContext* redis = connectRedis(host, port);
    if (!redis) {
        return 0;
    }

    std::cout << "controller started" << std::endl;

    while (!stopFlag) {
        DIR* dir = opendir("/inbox");
        if (dir) {
            struct dirent* entry = nullptr;
            while ((entry = readdir(dir)) != nullptr && !stopFlag) {
                std::string name = entry->d_name;
                if (name == "." || name == "..") {
                    continue;
                }
                std::string srcPath = "/inbox/" + name;
                struct stat st;
                if (stat(srcPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
                    continue;
                }
                redisReply* seen = static_cast<redisReply*>(
                    redisCommand(redis, "SADD seen_files %s", name.c_str())
                );
                if (!seen) {
                    redisFree(redis);
                    redis = connectRedis(host, port);
                    if (!redis) {
                        break;
                    }
                    continue;
                }
                bool isNew = (seen->type == REDIS_REPLY_INTEGER && seen->integer == 1);
                freeReplyObject(seen);

                if (isNew) {
                    std::string dstPath = "/data/in/" + name;
                    if (copyFile(srcPath, dstPath)) {
                        redisReply* push = static_cast<redisReply*>(
                            redisCommand(redis, "RPUSH tasks %s", name.c_str())
                        );
                        if (push) {
                            freeReplyObject(push);
                            std::cout << "[QUEUED] " << name << std::endl;
                        }
                    }
                }
            }
            closedir(dir);
        }

        if (stopFlag || !redis) {
            break;
        }

        redisReply* res = static_cast<redisReply*>(redisCommand(redis, "BLPOP results 1"));
        if (!res) {
            if (stopFlag) {
                break;
            }
            redisFree(redis);
            redis = connectRedis(host, port);
            continue;
        }
        if (res->type == REDIS_REPLY_ARRAY && res->elements == 2) {
            std::cout << "[DONE] " << res->element[1]->str << std::endl;
            freeReplyObject(res);
            while (!stopFlag) {
                redisReply* more = static_cast<redisReply*>(redisCommand(redis, "LPOP results"));
                if (!more) {
                    break;
                }
                bool hasItem = (more->type == REDIS_REPLY_STRING);
                if (hasItem) {
                    std::cout << "[DONE] " << more->str << std::endl;
                }
                freeReplyObject(more);
                if (!hasItem) {
                    break;
                }
            }
        } else {
            freeReplyObject(res);
        }
    }

    if (redis) {
        redisFree(redis);
    }
    std::cout << "controller stopped" << std::endl;
    return 0;
}