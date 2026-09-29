#include<unistd.h>
#include<filesystem>
#include<string>
#include<unordered_map>
#include<optional>
#include<sstream>
#include<fstream>
#include<csignal>
#include <arpa/inet.h>
#include<sys/socket.h>
#include<iostream>
namespace fs = std::filesystem;

class UniqueFd {
    int fd_ = -1;
    
public:
    UniqueFd() = default;
    
    explicit UniqueFd(int fd): fd_(fd) {}

    ~UniqueFd() {
        if(fd_ >= 0) close(fd_);
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if(this != &other) {
            if(fd_ >= 0) close(fd_);
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int get() const { return fd_; }
    bool is_valid() const { return fd_ >= 0; }
};

struct HttpRequest {
    std::string method;
    std::string target;
    std::string version;
    std::unordered_map<std::string, std::string> headers;

    static std::optional<HttpRequest> parse(std::string_view rawRequest) {
        if(rawRequest.empty()) return std::nullopt;
        std::istringstream stream{std::string(rawRequest)};
        HttpRequest req;
        if(!(stream >> req.method >> req.target >> req.version)) {
            return std::nullopt;
        }
        std::string line;
        std::getline(stream, line);
        while(std::getline(stream, line) && line != "\r" && !line.empty()) {
            auto colonPos = line.find(':');
            if(colonPos != std::string::npos) {
                std::string key = line.substr(0, colonPos);
                auto valStartPos = line.find_first_not_of(" \t", colonPos + 1);
                auto valEndPos = line.find_last_not_of("\r\n");
                if(valStartPos != std::string::npos && valEndPos >= valStartPos) {
                    req.headers[std::move(key)] = line.substr(valStartPos, valEndPos - valStartPos + 1);
                } 
            }
        }
        return req;
    }
};

class HttpResponse {
    int statusCode_ = 200;
    std::string statusText_ = "OK";
    std::unordered_map<std::string, std::string> headers_;
    std::vector<char> body_;

public:
    HttpResponse(int statusCode, std::string statusText)
        : statusCode_(statusCode), statusText_(std::move(statusText)) {
        headers_["Server"] = "MyHttpCppServer/1.0";
    }

    void setKeepAlive(bool keepAlive) {
        if (keepAlive) {
            headers_["Connection"] = "keep-alive";
            headers_["Keep-Alive"] = "timeout=5, max=100";
        } else {
            headers_["Connection"] = "close";
        }
    }

    void setHeader(std::string key, std::string value) {
        headers_[std::move(key)] = std::move(value);
    }

    void setBody(std::vector<char> body, const std::string& content_type) {
        body_ = std::move(body);
        setHeader("Content-Type", content_type);
        setHeader("Content-Length", std::to_string(body_.size()));
    }

    void setBody(const std::string& text_body, const std::string& content_type) {
        setBody(std::vector<char>(text_body.begin(), text_body.end()), content_type);
    }

    std::vector<char> serialize() const {
        std::ostringstream headerStream;
        headerStream << "HTTP/1.1 " << statusCode_ << " " << statusText_ << "\r\n";
        for(const auto& [key, value]: headers_) {
            headerStream << key << ": " << value << "\r\n";
        }
        headerStream << "\r\n";
        std::string header = headerStream.str();
        std::vector<char> wireData(header.begin(), header.end());
        wireData.insert(wireData.end(), body_.begin(), body_.end());
        return wireData;
    }

    static HttpResponse makeError(int code, const std::string& title, const std::string& content, bool keepAlive = false) {
        HttpResponse res(code, title);
        res.setKeepAlive(keepAlive);
        std::string body = "<html><body><h1>" + std::to_string(code) + " " + title + 
                           "</h1><p>" + content + "</p></body></html>";
        res.setBody(body, "text/html");
        return res;
    }
};

class StaticFileHandler {
    fs::path publicRoot_;
    static std::string getMimeType(const fs::path& filePath) {
        std::string extension = filePath.extension().string();
        if (extension == ".html" || extension == ".htm") return "text/html";
        if (extension == ".css")  return "text/css";
        if (extension == ".js")   return "application/javascript";
        if (extension == ".png")  return "image/png";
        if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
        if (extension == ".gif")  return "image/gif";
        if (extension == ".ico")  return "image/x-icon";
        if (extension == ".json") return "application/json";
        return "text/plain";
    }

public:
    explicit StaticFileHandler(fs::path root): publicRoot_(fs::canonical(root)) {}
    
    HttpResponse handle(const HttpRequest &req) const {
        if (req.method != "GET") {
            return HttpResponse::makeError(501, "Not Implemented", "Only GET method is supported.");
        }
        
        std::string relative_str = (req.target == "/") ? "/index.html" : req.target;

        if(!relative_str.empty() && relative_str.front() == '/') {
            relative_str = relative_str.substr(1);
        }

        fs::path requestedPath = (publicRoot_ / relative_str).lexically_normal();

        auto [rootEnd, mismatch] = std::mismatch(publicRoot_.begin(), publicRoot_.end(), requestedPath.begin());
        if (rootEnd != publicRoot_.end()) {
            return HttpResponse::makeError(403, "Forbidden", "Access denied.");
        }

        if (!fs::exists(requestedPath) || !fs::is_regular_file(requestedPath)) {
            return HttpResponse::makeError(404, "Not Found", "The requested file was not found.");
        }

        std::ifstream file(requestedPath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return HttpResponse::makeError(500, "Internal Server Error", "Could not open file.");
        }

        std::streamsize file_size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<char> file_data(static_cast<size_t>(file_size));
        if (!file.read(file_data.data(), file_size)) {
            return HttpResponse::makeError(500, "Internal Server Error", "Failed to read file.");
        }

        HttpResponse res(200, "OK");
        res.setBody(std::move(file_data), getMimeType(requestedPath));
        return res;
    }
};

class TcpServer {
    int port_;
    StaticFileHandler handler_;
    UniqueFd listenFd_;

    static bool sendAll(int socketFd, const char* data, size_t totalBytes) {
        size_t totalSent = 0;
        while(totalSent < totalBytes) {
            ssize_t sent = write(socketFd, data + totalSent, totalBytes - totalSent);
            if(sent < 0) {
                if(errno == EINTR) continue;
                return false;
            }
            if(sent == 0) return false;
            totalSent += static_cast<size_t>(sent);
        }
        return true;
    }

    void handleClient(UniqueFd clientFd) {
        int requestCount = 0;
        const int maxRequestsPerConnection = 100;
        struct timeval tv{};
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(clientFd.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        while(requestCount < maxRequestsPerConnection) {
            char buffer[4096];
            ssize_t bytesRead = read(clientFd.get(), buffer, sizeof(buffer)-1);
            if(bytesRead <= 0) {
                return;
            }

            buffer[bytesRead] = '\0';

            auto req = HttpRequest::parse(buffer);
            if (!req) {
                HttpResponse res = HttpResponse::makeError(400, "Bad Request", "Malformed HTTP request.", false);
                std::vector<char> wireData = res.serialize();
                sendAll(clientFd.get(), wireData.data(), wireData.size());
                break;
            }
            requestCount++;
            bool clientWantsClose = false;
            auto connectionHeader = req->headers.find("Connection");
            if (connectionHeader != req->headers.end()) {
                std::string val = connectionHeader->second;
                if (val.find("close") != std::string::npos || val.find("Close") != std::string::npos) {
                    clientWantsClose = true;
                }
            }   
            bool keepAlive = !clientWantsClose && (requestCount < maxRequestsPerConnection);
            HttpResponse res = handler_.handle(*req);
            res.setKeepAlive(keepAlive);
            std::vector<char> wireData = res.serialize();
            if (!sendAll(clientFd.get(), wireData.data(), wireData.size())) {
                break;
            }

            if (!keepAlive) {
                break;
            }
        }
    }

public:
    TcpServer(int port, fs::path publicDir): port_(port), handler_(std::move(publicDir)) {}
    
    bool start() {
        std::signal(SIGPIPE, SIG_IGN);
        int rawFd = socket(AF_INET, SOCK_STREAM, 0);
        if(rawFd < 0) {
            return false;
        }
        listenFd_ = UniqueFd(rawFd);
        int opt = 1;
        if(setsockopt(listenFd_.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if(bind(listenFd_.get(), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            return false;
        }

        if(listen(listenFd_.get(), 64) < 0) {
            return false;
        }

        std::cout << "Server listening on http://localhost:" << port_ << "\n";
        return true;
    }

    void run() {
        while(true) {
            sockaddr_in clientAddr{};
            socklen_t clientLen = sizeof(clientAddr);
            int clientRaw = accept(listenFd_.get(), reinterpret_cast<struct sockaddr*>(&clientAddr), &clientLen);
            if(clientRaw < 0) {
                if(errno == EINTR) continue;
                continue;
            }

            UniqueFd clientFd(clientRaw);
            handleClient(std::move(clientFd));
        }
    }
};

int main() {
    fs::create_directories("./public");
    TcpServer server(8080, "./public");
    if (!server.start()) {
        return 1;
    }
    server.run();
    return 0;
}