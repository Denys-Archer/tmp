#include "downloader.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <netdb.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>

namespace fs = std::filesystem;

namespace bavovna::downloader {
namespace {

struct Url {
	std::string host;
	std::string port;
	std::string authority;
	std::string target;
};

std::string trim(std::string value) {
	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return {};
	const auto last = value.find_last_not_of(" \t\r\n");
	return value.substr(first, last - first + 1);
}

std::string lowercase(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return value;
}

Url parse_url(const std::string& text) {
	constexpr const char* scheme = "http://";
	if (text.rfind(scheme, 0) != 0) {
		throw std::runtime_error("Only HTTP URLs are supported (http://): " + text);
	}

	const auto authority_start = std::char_traits<char>::length(scheme);
	const auto path_start = text.find_first_of("/?#", authority_start);
	const auto authority = text.substr(authority_start,
		path_start == std::string::npos ? std::string::npos : path_start - authority_start);
	if (authority.empty() || authority.find('@') != std::string::npos) {
		throw std::runtime_error("Invalid HTTP server address: " + text);
	}

	Url result;
	result.authority = authority;
	result.port = "80";
	if (authority.front() == '[') {
		const auto closing = authority.find(']');
		if (closing == std::string::npos) throw std::runtime_error("Invalid IPv6 address: " + text);
		result.host = authority.substr(1, closing - 1);
		if (closing + 1 < authority.size()) {
			if (authority[closing + 1] != ':') throw std::runtime_error("Invalid URL port: " + text);
			result.port = authority.substr(closing + 2);
		}
	} else {
		const auto colon = authority.rfind(':');
		if (colon == std::string::npos) {
			result.host = authority;
		} else {
			if (authority.find(':') != colon) throw std::runtime_error("IPv6 addresses must be enclosed in brackets.");
			result.host = authority.substr(0, colon);
			result.port = authority.substr(colon + 1);
		}
	}
	if (result.host.empty() || result.port.empty()) throw std::runtime_error("Invalid host or port in URL: " + text);

	if (path_start == std::string::npos || text[path_start] == '#') {
		result.target = "/";
	} else if (text[path_start] == '?') {
		result.target = "/" + text.substr(path_start, text.find('#', path_start) - path_start);
	} else {
		const auto fragment = text.find('#', path_start);
		result.target = text.substr(path_start, fragment == std::string::npos ? fragment : fragment - path_start);
	}
	return result;
}

class Socket {
public:
	explicit Socket(const Url& url) {
		addrinfo hints{};
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		addrinfo* addresses = nullptr;
		const int lookup = getaddrinfo(url.host.c_str(), url.port.c_str(), &hints, &addresses);
		if (lookup != 0) throw std::runtime_error("Could not resolve server " + url.host + ": " + gai_strerror(lookup));

		for (auto* address = addresses; address != nullptr; address = address->ai_next) {
			fd_ = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
			if (fd_ < 0) continue;
			timeval timeout{60, 0};
			setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
			setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
			if (connect(fd_, address->ai_addr, address->ai_addrlen) == 0) break;
			close(fd_);
			fd_ = -1;
		}
		freeaddrinfo(addresses);
		if (fd_ < 0) throw std::runtime_error("Could not connect to " + url.host + ":" + url.port);
	}

	~Socket() {
		if (fd_ >= 0) close(fd_);
	}

	Socket(const Socket&) = delete;
	Socket& operator=(const Socket&) = delete;
	int fd() const { return fd_; }

private:
	int fd_ = -1;
};

class Reader {
public:
	explicit Reader(int fd) : fd_(fd) {}

	bool read_line(std::string& line) {
		for (;;) {
			const auto newline = buffer_.find('\n');
			if (newline != std::string::npos) {
				line = buffer_.substr(0, newline);
				buffer_.erase(0, newline + 1);
				if (!line.empty() && line.back() == '\r') line.pop_back();
				return true;
			}
			if (!fill()) {
				if (buffer_.empty()) return false;
				line = std::move(buffer_);
				buffer_.clear();
				return true;
			}
		}
	}

	template <typename Callback>
	void read_exact(std::uint64_t size, Callback&& callback) {
		while (size > 0) {
			if (buffer_.empty() && !fill()) throw std::runtime_error("Connection closed during download.");
			const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(size, buffer_.size()));
			callback(buffer_.data(), amount);
			buffer_.erase(0, amount);
			size -= amount;
		}
	}

	template <typename Callback>
	void read_to_end(Callback&& callback) {
		for (;;) {
			if (!buffer_.empty()) {
				callback(buffer_.data(), buffer_.size());
				buffer_.clear();
			}
			if (!fill()) return;
		}
	}

private:
	bool fill() {
		char chunk[65536];
		ssize_t received;
		do {
			received = recv(fd_, chunk, sizeof(chunk), 0);
		} while (received < 0 && errno == EINTR);
		if (received < 0) throw std::runtime_error("Failed to read the HTTP response.");
		if (received == 0) return false;
		buffer_.append(chunk, static_cast<std::size_t>(received));
		return true;
	}

	int fd_;
	std::string buffer_;
};

using Headers = std::map<std::string, std::string>;

void send_all(int fd, const std::string& request) {
	std::size_t sent = 0;
	while (sent < request.size()) {
		const auto count = send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) throw std::runtime_error("Failed to send the HTTP request.");
		sent += static_cast<std::size_t>(count);
	}
}

std::string resolve_redirect(const Url& current, const std::string& location) {
	if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) return location;
	const std::string origin = "http://" + current.authority;
	if (location.rfind("//", 0) == 0) return "http:" + location;
	if (!location.empty() && location.front() == '/') return origin + location;
	if (!location.empty() && location.front() == '?') {
		const auto query_start = current.target.find('?');
		return origin + current.target.substr(0, query_start) + location;
	}
	const auto slash = current.target.rfind('/');
	return origin + current.target.substr(0, slash == std::string::npos ? 0 : slash + 1) + location;
}

template <typename Callback>
void stream_body(Reader& reader, const Headers& headers, Callback&& callback) {
	const auto transfer = headers.find("transfer-encoding");
	if (transfer != headers.end() && lowercase(transfer->second).find("chunked") != std::string::npos) {
		for (;;) {
			std::string line;
			if (!reader.read_line(line)) throw std::runtime_error("Invalid chunked HTTP response.");
			const auto extension = line.find(';');
			const auto size_text = line.substr(0, extension);
			std::uint64_t chunk_size = 0;
			try {
				chunk_size = std::stoull(size_text, nullptr, 16);
			} catch (...) {
				throw std::runtime_error("Invalid HTTP chunk size.");
			}
			if (chunk_size == 0) {
				while (reader.read_line(line) && !line.empty()) {}
				return;
			}
			reader.read_exact(chunk_size, callback);
			reader.read_exact(2, [](const char*, std::size_t) {});
		}
	}

	const auto length = headers.find("content-length");
	if (length != headers.end()) {
		std::uint64_t remaining = 0;
		try {
			remaining = std::stoull(length->second);
		} catch (...) {
			throw std::runtime_error("Invalid Content-Length in HTTP response.");
		}
		reader.read_exact(remaining, callback);
		return;
	}
	reader.read_to_end(callback);
}

template <typename Callback>
void request(const std::string& initial_url, Callback&& body_callback) {
	std::string url_text = initial_url;
	for (int redirect_count = 0; redirect_count <= 8; ++redirect_count) {
		const auto url = parse_url(url_text);
		Socket socket(url);
		const std::string request_text = "GET " + url.target + " HTTP/1.1\r\nHost: " + url.authority +
			"\r\nUser-Agent: bavovna-package-manager/1.0\r\nAccept: */*\r\nConnection: close\r\n\r\n";
		send_all(socket.fd(), request_text);
		Reader reader(socket.fd());

		std::string status_line;
		if (!reader.read_line(status_line)) throw std::runtime_error("Empty response from HTTP server.");
		std::istringstream status_stream(status_line);
		std::string protocol;
		int status = 0;
		status_stream >> protocol >> status;
		if (status < 100 || status > 599) throw std::runtime_error("Invalid HTTP status: " + status_line);

		Headers headers;
		std::string line;
		while (reader.read_line(line) && !line.empty()) {
			const auto colon = line.find(':');
			if (colon == std::string::npos) continue;
			headers[lowercase(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
		}

		if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
			const auto location = headers.find("location");
			if (location == headers.end()) throw std::runtime_error("HTTP redirect is missing the Location header.");
			if (redirect_count == 8) throw std::runtime_error("Too many HTTP redirects.");
			url_text = resolve_redirect(url, location->second);
			continue;
		}
		if (status < 200 || status >= 300) {
			throw std::runtime_error("HTTP server returned status " + std::to_string(status) + ".");
		}

		body_callback(reader, headers);
		return;
	}
}

void draw_progress(const std::string& label, std::uint64_t downloaded, std::uint64_t total) {
	const auto percent = total == 0 ? 0 : static_cast<unsigned>(std::min<std::uint64_t>(100, downloaded * 100 / total));
	constexpr int width = 32;
	const auto filled = static_cast<int>(percent * width / 100);
	std::cerr << '\r' << label << " [" << std::string(filled, '=') << std::string(width - filled, ' ')
			  << "] " << std::setw(3) << percent << "%";
	if (total > 0) std::cerr << " (" << downloaded << "/" << total << " bytes)";
	else std::cerr << " (" << downloaded << " bytes)";
	std::cerr << "\033[K" << std::flush;
}

}

std::string get(const std::string& url) {
	std::string response;
	request(url, [&](Reader& reader, const Headers& headers) {
		stream_body(reader, headers, [&](const char* data, std::size_t size) {
			constexpr std::size_t max_response_size = 32 * 1024 * 1024;
			if (size > max_response_size - response.size()) throw std::runtime_error("HTTP response is too large.");
			response.append(data, size);
		});
	});
	return response;
}

void download(const std::string& url, const fs::path& destination, const std::string& progress_label) {
	const fs::path temporary = destination.string() + ".part";
	const bool interactive = isatty(STDERR_FILENO);
	try {
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if (!output) throw std::runtime_error("Could not create download file: " + temporary.string());
		request(url, [&](Reader& reader, const Headers& headers) {
			std::uint64_t total = 0;
			const auto length = headers.find("content-length");
			if (length != headers.end()) {
				try {
					total = std::stoull(length->second);
				} catch (...) {
					throw std::runtime_error("Invalid Content-Length in package response.");
				}
			}
			std::uint64_t downloaded = 0;
			unsigned last_percent = 101;
			if (interactive) draw_progress(progress_label, 0, total);
			stream_body(reader, headers, [&](const char* data, std::size_t size) {
				output.write(data, static_cast<std::streamsize>(size));
				if (!output) throw std::runtime_error("Failed to write downloaded package.");
				downloaded += size;
				const auto percent = total == 0 ? 0 : static_cast<unsigned>(std::min<std::uint64_t>(100, downloaded * 100 / total));
				if (interactive && (total == 0 || percent != last_percent)) {
					draw_progress(progress_label, downloaded, total);
					last_percent = percent;
				}
			});
			output.flush();
			if (!output) throw std::runtime_error("Failed to finish writing downloaded package.");
			if (total > 0 && downloaded != total) throw std::runtime_error("Downloaded size does not match Content-Length.");
			if (interactive) {
				draw_progress(progress_label, downloaded, total == 0 ? downloaded : total);
				std::cerr << '\n';
			}
		});
		output.close();
		if (!output) throw std::runtime_error("Failed to close downloaded file.");
		fs::rename(temporary, destination);
	} catch (...) {
		std::error_code ignored;
		fs::remove(temporary, ignored);
		throw;
	}
}

}
