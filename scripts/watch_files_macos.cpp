#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits.h>
#include <set>
#include <sstream>
#include <string>
#include <sys/event.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifndef O_EVTONLY
#define O_EVTONLY O_RDONLY
#endif

static std::string absolutePath(const std::string& path) {
    if (!path.empty() && path[0] == '/') return path;

    char cwd[PATH_MAX] = {};
    if (!getcwd(cwd, sizeof(cwd))) return path;
    return std::string(cwd) + "/" + path;
}

static std::string parentDir(const std::string& path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

static std::string signature(const std::vector<std::string>& paths) {
    std::ostringstream out;
    for (const std::string& path : paths) {
        struct stat st {};
        if (lstat(path.c_str(), &st) == 0) {
            out << path << ':' << st.st_ino << ':' << st.st_size << ':'
                << st.st_mtimespec.tv_sec << ':' << st.st_mtimespec.tv_nsec << '\n';
        } else {
            out << path << ":missing\n";
        }
    }
    return out.str();
}

static void closeWatches(std::vector<int>& fds) {
    for (int fd : fds) close(fd);
    fds.clear();
}

static void addWatch(int kq, std::vector<int>& fds, const std::string& path, bool required) {
    int fd = open(path.c_str(), O_EVTONLY);
    if (fd < 0) {
        if (required) {
            std::cerr << "watch_files_macos: cannot watch " << path
                      << ": " << std::strerror(errno) << "\n";
        }
        return;
    }

    struct kevent change {};
    EV_SET(&change, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_DELETE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_RENAME | NOTE_REVOKE,
           0, nullptr);
    if (kevent(kq, &change, 1, nullptr, 0, nullptr) < 0) {
        std::cerr << "watch_files_macos: kevent failed for " << path
                  << ": " << std::strerror(errno) << "\n";
        close(fd);
        return;
    }
    fds.push_back(fd);
}

static void rebuildWatches(int kq,
                           std::vector<int>& fds,
                           const std::vector<std::string>& paths,
                           const std::vector<std::string>& dirs) {
    closeWatches(fds);

    for (const std::string& path : paths) {
        addWatch(kq, fds, path, false);
    }
    for (const std::string& dir : dirs) {
        addWatch(kq, fds, dir, true);
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: watch_files_macos <file> [more-files...]\n";
        return 2;
    }

    std::vector<std::string> paths;
    std::set<std::string> uniqueDirs;
    for (int i = 1; i < argc; i++) {
        std::string path = absolutePath(argv[i]);
        paths.push_back(path);
        uniqueDirs.insert(parentDir(path));
    }
    std::vector<std::string> dirs(uniqueDirs.begin(), uniqueDirs.end());

    int kq = kqueue();
    if (kq < 0) {
        std::cerr << "watch_files_macos: kqueue failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    std::vector<int> fds;
    std::string currentSignature = signature(paths);
    rebuildWatches(kq, fds, paths, dirs);

    while (true) {
        struct kevent event {};
        int n = kevent(kq, nullptr, 0, &event, 1, nullptr);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::cerr << "watch_files_macos: kevent wait failed: "
                      << std::strerror(errno) << "\n";
            closeWatches(fds);
            close(kq);
            return 1;
        }

        usleep(250000);
        struct timespec zero {0, 0};
        while (kevent(kq, nullptr, 0, &event, 1, &zero) > 0) {}

        std::string nextSignature = signature(paths);
        if (nextSignature != currentSignature) {
            currentSignature = nextSignature;
            std::cout << "change\n" << std::flush;
        }

        rebuildWatches(kq, fds, paths, dirs);
    }
}
