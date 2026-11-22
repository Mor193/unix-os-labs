#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <openssl/evp.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

struct FileEntry {
    std::string path;
    dev_t dev;
    ino_t ino;
};

void collect_files(const std::string& dir_path, std::vector<FileEntry>& files) {
    DIR* dir = opendir(dir_path.c_str());
    if (!dir) {
        std::cerr << "Cannot open directory " << dir_path << ": " << std::strerror(errno) << '\n';
        return;
    }

    while (struct dirent* entry = readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        std::string full_path = (dir_path == "/")
                                    ? ("/" + std::string(entry->d_name))
                                    : (dir_path + "/" + entry->d_name);

        struct stat st;
        if (lstat(full_path.c_str(), &st) != 0) {
            std::cerr << "lstat failed for " << full_path << ": " << std::strerror(errno) << '\n';
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            collect_files(full_path, files);
        } else if (S_ISREG(st.st_mode)) {
            files.push_back({full_path, st.st_dev, st.st_ino});
        }
    }

    closedir(dir);
}

bool compute_sha1(const std::string& path, std::string& out_hash) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        close(fd);
        return false;
    }

    if (EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        close(fd);
        return false;
    }

    unsigned char buf[65536];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            if (EVP_DigestUpdate(ctx, buf, static_cast<size_t>(n)) != 1) {
                EVP_MD_CTX_free(ctx);
                close(fd);
                return false;
            }
        } else if (n == 0) {
            break;
        } else {
            if (errno == EINTR) {
                continue;
            }
            EVP_MD_CTX_free(ctx);
            close(fd);
            return false;
        }
    }

    close(fd);

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return false;
    }

    EVP_MD_CTX_free(ctx);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digest_len; ++i) {
        oss << std::setw(2) << static_cast<int>(digest[i]);
    }
    out_hash = oss.str();
    return true;
}

bool replace_with_hardlink(const std::string& target, const std::string& duplicate) {
    std::size_t pos = duplicate.find_last_of('/');
    std::string dir;
    if (pos == std::string::npos) {
        dir = ".";
    } else if (pos == 0) {
        dir = "/";
    } else {
        dir = duplicate.substr(0, pos);
    }

    std::string tmp_path;
    unsigned long long counter = 0;
    while (true) {
        tmp_path = (dir == "/" ? "" : dir) + "/.hl_tmp_" + std::to_string(getpid()) + "_" + std::to_string(counter++);
        if (link(target.c_str(), tmp_path.c_str()) == 0) {
            break;
        }
        if (errno != EEXIST) {
            return false;
        }
    }

    if (std::rename(tmp_path.c_str(), duplicate.c_str()) != 0) {
        int saved_errno = errno;
        unlink(tmp_path.c_str());
        errno = saved_errno;
        return false;
    }

    return true;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <directory>\n";
        return 1;
    }

    std::string root = argv[1];
    while (root.size() > 1 && root.back() == '/') {
        root.pop_back();
    }

    struct stat root_st;
    if (stat(root.c_str(), &root_st) != 0 || !S_ISDIR(root_st.st_mode)) {
        std::cerr << "Invalid directory: " << root << '\n';
        return 1;
    }

    std::vector<FileEntry> files;
    collect_files(root, files);
    std::sort(files.begin(), files.end(), [](const FileEntry& a, const FileEntry& b) {
        return a.path < b.path;
    });

    std::unordered_map<std::string, FileEntry> seen;
    std::size_t linked_count = 0;

    for (const auto& file : files) {
        std::string hash;
        if (!compute_sha1(file.path, hash)) {
            std::cerr << "Skip " << file.path << ": " << std::strerror(errno) << '\n';
            continue;
        }

        std::string key = std::to_string(file.dev) + ":" + hash;
        auto [it, inserted] = seen.emplace(key, file);
        if (!inserted) {
            if (it->second.ino == file.ino) {
                continue;
            }

            if (replace_with_hardlink(it->second.path, file.path)) {
                ++linked_count;
                std::cout << "Linked: " << file.path << " -> " << it->second.path << '\n';
            } else {
                std::cerr << "Failed to link " << file.path << ": " << std::strerror(errno) << '\n';
            }
        }
    }

    std::cout << "Scanned files: " << files.size() << '\n';
    std::cout << "Unique (fs + hash): " << seen.size() << '\n';
    std::cout << "Replaced with hard links: " << linked_count << '\n';

    return 0;
}