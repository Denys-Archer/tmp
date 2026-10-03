#include "check.hpp"
#include "downloader.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <set>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

namespace bavovna {
namespace {

using Clock = std::chrono::steady_clock;

std::string trim(std::string value) {
	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return {};
	const auto last = value.find_last_not_of(" \t\r\n");
	return value.substr(first, last - first + 1);
}

bool valid_package_name(const std::string& name) {
	return !name.empty() && name != "." && name != ".." &&
		std::all_of(name.begin(), name.end(), [](unsigned char ch) {
			return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '+';
		});
}

std::string run_capture(const std::vector<std::string>& args) {
	int pipes[2];
	if (pipe(pipes) != 0) throw std::runtime_error("Failed to create a pipe.");

	const pid_t child = fork();
	if (child < 0) {
		close(pipes[0]);
		close(pipes[1]);
		throw std::runtime_error("Failed to start a process.");
	}
	if (child == 0) {
		close(pipes[0]);
		dup2(pipes[1], STDOUT_FILENO);
		dup2(pipes[1], STDERR_FILENO);
		close(pipes[1]);
		std::vector<char*> argv;
		for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
		argv.push_back(nullptr);
		execvp(argv[0], argv.data());
		_exit(127);
	}

	close(pipes[1]);
	std::string output;
	char buffer[8192];
	ssize_t count;
	while ((count = read(pipes[0], buffer, sizeof(buffer))) > 0) {
		output.append(buffer, static_cast<std::size_t>(count));
	}
	close(pipes[0]);

	int status = 0;
	while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		throw std::runtime_error(trim(output).empty() ? "A command failed." : trim(output));
	}
	return output;
}

void draw_progress(const std::string& label, std::size_t completed, std::size_t total) {
	const auto percent = total == 0 ? 100 : std::min<std::size_t>(100, completed * 100 / total);
	const auto width = 24;
	const auto filled = static_cast<int>(percent * width / 100);
	std::cerr << '\r' << label << " [" << std::string(filled, '=') << std::string(width - filled, ' ')
			  << "] " << std::setw(3) << percent << "% (" << std::min(completed, total) << "/" << total
			  << " files)" << std::flush;
}

void extract_with_progress(const std::vector<std::string>& args, std::size_t total_files,
						   const std::string& package_name) {
	int pipes[2];
	if (pipe(pipes) != 0) throw std::runtime_error("Failed to create an extraction pipe.");

	const pid_t child = fork();
	if (child < 0) {
		close(pipes[0]);
		close(pipes[1]);
		throw std::runtime_error("Failed to start extraction.");
	}
	if (child == 0) {
		close(pipes[0]);
		dup2(pipes[1], STDOUT_FILENO);
		dup2(pipes[1], STDERR_FILENO);
		close(pipes[1]);
		std::vector<char*> argv;
		for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
		argv.push_back(nullptr);
		execvp(argv[0], argv.data());
		_exit(127);
	}

	close(pipes[1]);
	const bool interactive = isatty(STDERR_FILENO);
	std::size_t completed = 0;
	std::string pending;
	std::string diagnostic_text;
	char buffer[8192];
	ssize_t count;
	if (interactive) draw_progress("Extracting " + package_name, 0, total_files);
	auto process_line = [&](const std::string& line) {
		if (line.rfind("x ", 0) == 0) {
			++completed;
			if (interactive) draw_progress("Extracting " + package_name, completed, total_files);
		} else if (!line.empty()) {
			diagnostic_text += line + '\n';
		}
	};
	while ((count = read(pipes[0], buffer, sizeof(buffer))) > 0) {
		pending.append(buffer, static_cast<std::size_t>(count));
		std::size_t newline;
		while ((newline = pending.find('\n')) != std::string::npos) {
			process_line(pending.substr(0, newline));
			pending.erase(0, newline + 1);
		}
	}
	if (!pending.empty()) process_line(pending);
	close(pipes[0]);

	int status = 0;
	while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
	if (interactive) std::cerr << '\r' << "\033[K" << std::flush;
	const bool completed_all_files = completed >= total_files;
	const bool completed_with_warning = WIFEXITED(status) && WEXITSTATUS(status) == 1 && completed_all_files;
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0 && !completed_with_warning)) {
		const auto diagnostic = trim(diagnostic_text);
		throw std::runtime_error(diagnostic.empty()
			? "Could not fully extract package " + package_name
			: "Could not fully extract package " + package_name + ": " + diagnostic);
	}
	if (interactive) draw_progress("Extracting " + package_name, total_files, total_files);
	if (interactive) std::cerr << '\n';
}

std::string safe_member_path(std::string member) {
	if (member.empty() || member.front() == '/') throw std::runtime_error("Archive contains an absolute path.");
	while (member.rfind("./", 0) == 0) member.erase(0, 2);
	if (!member.empty() && member.back() == '/') member.pop_back();
	if (member.empty()) return {};
	const fs::path path(member);
	for (const auto& part : path) {
		if (part == "..") throw std::runtime_error("Archive contains an unsafe path: " + member);
	}
	return path.lexically_normal().generic_string();
}

std::string archive_metadata(const fs::path& archive, const std::string& member) {
	try {
		return run_capture({"bsdtar", "-xOf", archive.string(), member});
	} catch (const std::exception&) {
		return {};
	}
}

void analyze_package_metadata(const fs::path& archive, const std::vector<std::string>& members,
							  const std::string& package_name, bool inspect_install_script) {
	const auto has_member = [&](const std::string& name) {
		return std::find(members.begin(), members.end(), name) != members.end();
	};

	bool found_metadata = false;
	if (has_member(".PKGINFO")) {
		const auto contents = archive_metadata(archive, ".PKGINFO");
		if (!contents.empty()) {
			found_metadata = true;
			std::cerr << "Package metadata for " << package_name << ":\n";
			std::istringstream input(contents);
			std::string line;
			while (std::getline(input, line)) {
				line = trim(line);
				if (line.empty() || line.front() == '#') continue;
				const auto separator = line.find('=');
				if (separator == std::string::npos) continue;
				const auto key = trim(line.substr(0, separator));
				const auto value = trim(line.substr(separator + 1));
				if (key == "pkgname" || key == "pkgver" || key == "pkgdesc" || key == "arch" ||
					key == "url" || key == "size" || key == "depend" || key == "optdepend" || key == "conflict" ||
					key == "provides" || key == "replaces") {
					std::cerr << "  " << key << ": " << value << '\n';
				}
			}
		}
	}

	if (inspect_install_script) {
		std::string install_script_name;
		for (const auto& candidate : {std::string(".PKGINSTALL"), std::string(".INSTALL")}) {
			if (has_member(candidate)) {
				install_script_name = candidate;
				break;
			}
		}
		if (!install_script_name.empty()) {
			const auto script = archive_metadata(archive, install_script_name);
			if (!script.empty()) {
				found_metadata = true;
				std::cerr << "  Script " << install_script_name << ": found, " << script.size() << " bytes.\n";
				for (const auto* hook : {"pre_install", "post_install", "pre_upgrade", "post_upgrade",
										 "pre_remove", "post_remove", "pre_transaction", "post_transaction"}) {
					if (script.find(hook) != std::string::npos) {
						std::cerr << "    lifecycle hook: " << hook << '\n';
					}
				}
			}
		}
	}

	if (!found_metadata) std::cerr << "No .PKGINFO/.PKGINSTALL metadata found for " << package_name << ".\n";
}

fs::path package_database(const fs::path& directory) {
	return directory / ".local" / "state" / "bavovna" / "packages";
}

struct PackageOptions {
	bool dependencies = false;
	bool pluscommands = false;
};

bool parse_switch(const std::map<std::string, std::string>& options, const std::string& key) {
	const auto found = options.find(key);
	if (found == options.end()) return false;
	const auto value = lowercase(trim(found->second));
	if (value == "yes") return true;
	if (value == "no") return false;
	throw std::runtime_error("The .PKGCONFIG value for " + key + " must be yes or no.");
}

PackageOptions read_package_options(const std::string& config) {
	std::map<std::string, std::string> values;
	std::istringstream input(config);
	std::string line;
	while (std::getline(input, line)) {
		line = trim(line);
		if (line.empty() || line.front() == '#') continue;
		const auto separator = line.find('=');
		if (separator == std::string::npos) continue;
		values[lowercase(trim(line.substr(0, separator)))] = trim(line.substr(separator + 1));
	}
	return {parse_switch(values, "dependencies"), parse_switch(values, "pluscommands")};
}

std::vector<std::string> read_dependencies(const std::string& contents) {
	std::vector<std::string> dependencies;
	std::istringstream input(contents);
	std::string line;
	while (std::getline(input, line)) {
		line = trim(line);
		if (line.empty() || line.front() == '#') continue;
		if (!valid_package_name(line)) throw std::runtime_error("Invalid dependency in .PKGDEPENDENCIES: " + line);
		dependencies.push_back(line);
	}
	return dependencies;
}

void run_install_script(const std::string& script, const std::string& package_name) {
	if (script.empty()) return;
	char path[] = "/tmp/bavovna-install-XXXXXX";
	const int fd = mkstemp(path);
	if (fd < 0) throw std::runtime_error("Failed to create a temporary .PKGINSTALL file in /tmp.");

	std::size_t written = 0;
	while (written < script.size()) {
		const auto count = write(fd, script.data() + written, script.size() - written);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) {
			close(fd);
			unlink(path);
			throw std::runtime_error("Failed to write the temporary .PKGINSTALL file.");
		}
		written += static_cast<std::size_t>(count);
	}
	if (close(fd) != 0) {
		unlink(path);
		throw std::runtime_error("Failed to close the temporary .PKGINSTALL file.");
	}

	std::cout << "Running package install commands for " << package_name << ":\n" << std::flush;
	const pid_t child = fork();
	if (child < 0) {
		unlink(path);
		throw std::runtime_error("Failed to run .PKGINSTALL.");
	}
	if (child == 0) {
		execl("/bin/bash", "bash", path, static_cast<char*>(nullptr));
		_exit(127);
	}
	int status = 0;
	while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
	unlink(path);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		throw std::runtime_error(".PKGINSTALL failed for package " + package_name + ".");
	}
}

void install_package_recursive(const Package& package, const std::string& repo, const fs::path& directory,
							   const std::vector<Package>& repository_packages, std::set<std::string>& installing);

}

struct Activity::State {
	explicit State(std::string text) : message(std::move(text)), started(Clock::now()), interactive(isatty(STDERR_FILENO)) {
		if (interactive) {
			worker = std::thread([this] {
				static constexpr char frames[] = {'|', '/', '-', '\\'};
				std::size_t frame = 0;
				while (running.load()) {
					std::cerr << '\r' << frames[frame++ % 4] << ' ' << message << "   " << std::flush;
					std::this_thread::sleep_for(std::chrono::milliseconds(80));
				}
			});
		}
	}

	std::string message;
	Clock::time_point started;
	bool interactive;
	std::atomic<bool> running{true};
	std::thread worker;
};

Activity::Activity(std::string message) : state_(std::make_unique<State>(std::move(message))) {}

Activity::~Activity() {
	if (state_) finish(false);
}

void Activity::finish(bool success) {
	if (!state_) return;
	if (state_->interactive) {
		const auto elapsed = Clock::now() - state_->started;
		const auto minimum = std::chrono::milliseconds(300);
		if (elapsed < minimum) std::this_thread::sleep_for(minimum - elapsed);
		state_->running = false;
		if (state_->worker.joinable()) state_->worker.join();
		std::cerr << '\r' << "  " << state_->message << (success ? " [ok]" : " [failed]")
				  << "\033[K\n" << std::flush;
	}
	state_.reset();
}

std::string lowercase(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return value;
}

std::string read_config(const fs::path& config_path) {
	std::ifstream config(config_path);
	if (!config) throw std::runtime_error("Could not open configuration file: " + config_path.string());

	std::string line;
	while (std::getline(config, line)) {
		line = trim(line);
		if (line.empty() || line.front() == '#') continue;
		const auto separator = line.find('=');
		if (separator == std::string::npos) continue;
		if (lowercase(trim(line.substr(0, separator))) == "repo") {
			auto url = trim(line.substr(separator + 1));
			while (!url.empty() && url.back() == '/') url.pop_back();
			if (url.empty()) break;
			return url;
		}
	}
	throw std::runtime_error("No repository URL is configured.");
}

std::string fetch_text(const std::string& url) {
	Activity activity("Fetching repository data");
	auto result = downloader::get(url);
	activity.finish();
	return result;
}

std::vector<Package> read_packages(const std::string& repo) {
	const std::string index = fetch_text(repo + "/info.txt");
	std::vector<Package> packages;
	std::string line;
	std::size_t start = 0;
	while (start <= index.size()) {
		const auto end = index.find('\n', start);
		line = trim(index.substr(start, end == std::string::npos ? end : end - start));
		if (!line.empty() && line.front() != '#') {
			const auto separator = line.find('=');
			if (separator != std::string::npos) {
				Package package{trim(line.substr(0, separator)), trim(line.substr(separator + 1))};
				const fs::path archive_path(package.archive);
				if (valid_package_name(package.name) && lowercase(package.name) != "news" &&
					!package.archive.empty() && !archive_path.is_absolute() &&
					std::none_of(archive_path.begin(), archive_path.end(), [](const fs::path& part) {
						return part == "..";
					})) {
					packages.push_back(std::move(package));
				}
			}
		}
		if (end == std::string::npos) break;
		start = end + 1;
	}
	return packages;
}

const Package& find_package(const std::vector<Package>& packages, const std::string& query) {
	const auto wanted = lowercase(query);
	const auto found = std::find_if(packages.begin(), packages.end(), [&](const Package& package) {
		return lowercase(package.name) == wanted;
	});
	if (found == packages.end()) throw std::runtime_error("Package not found: " + query);
	return *found;
}

std::vector<Package> search_packages(const std::vector<Package>& packages, const std::string& query) {
	Activity activity("Searching packages");
	const auto wanted = lowercase(query);
	std::vector<Package> matches;
	for (const auto& package : packages) {
		if (lowercase(package.name).find(wanted) != std::string::npos ||
			lowercase(package.archive).find(wanted) != std::string::npos) {
			matches.push_back(package);
		}
	}
	activity.finish();
	return matches;
}

namespace {

void install_package_recursive(const Package& package, const std::string& repo, const fs::path& directory,
							   const std::vector<Package>& repository_packages, std::set<std::string>& installing) {
	const auto database = package_database(directory);
	const auto manifest = database / (package.name + ".list");
	if (fs::exists(manifest)) return;
	const auto package_key = lowercase(package.name);
	if (!installing.insert(package_key).second) {
		throw std::runtime_error("Dependency cycle detected for package " + package.name + ".");
	}
	struct VisitingGuard {
		std::set<std::string>& packages;
		std::string name;
		~VisitingGuard() { packages.erase(name); }
	} visiting_guard{installing, package_key};

	const auto cache = directory / ".cache" / "bavovna";
	fs::create_directories(cache);
	fs::create_directories(database);
	const auto archive = cache / fs::path(package.archive).filename();
	const auto url = repo + "/" + package.archive;
	downloader::download(url, archive, "Downloading " + package.name);

	std::string listing;
	{
		Activity activity("Checking archive " + package.name);
		listing = run_capture({"bsdtar", "-tf", archive.string()});
		activity.finish();
	}
	std::vector<std::string> members;
	std::vector<std::string> archive_members;
	std::vector<std::string> excluded_metadata;
	std::size_t start = 0;
	while (start < listing.size()) {
		const auto end = listing.find('\n', start);
		const auto raw = listing.substr(start, end == std::string::npos ? end : end - start);
		const auto member = safe_member_path(raw);
		if (!member.empty()) archive_members.push_back(member);
		if (member == ".PKGINFO" || member == ".PKGCONFIG" || member == ".PKGDEPENDENCIES" ||
			member == ".PKGINSTALL" || member == ".BUILDINFO" || member == ".MTREE" ||
			member == ".INSTALL" || member == ".CHANGELOG") {
			excluded_metadata.push_back(member);
		} else if (!member.empty()) {
			members.push_back(member);
		}
		if (end == std::string::npos) break;
		start = end + 1;
	}
	if (members.empty()) throw std::runtime_error("Archive is empty: " + package.name);

	const auto has_member = [&](const std::string& name) {
		return std::find(archive_members.begin(), archive_members.end(), name) != archive_members.end();
	};
	PackageOptions options;
	if (has_member(".PKGCONFIG")) {
		options = read_package_options(archive_metadata(archive, ".PKGCONFIG"));
	}
	analyze_package_metadata(archive, archive_members, package.name, options.pluscommands);
	if (options.dependencies) {
		if (!has_member(".PKGDEPENDENCIES")) {
			throw std::runtime_error("dependencies=yes, but .PKGDEPENDENCIES is missing from " + package.name);
		}
		for (const auto& dependency_name : read_dependencies(archive_metadata(archive, ".PKGDEPENDENCIES"))) {
			const auto& dependency = find_package(repository_packages, dependency_name);
			std::cout << "Dependency " << package.name << " -> " << dependency.name << '\n';
			install_package_recursive(dependency, repo, directory, repository_packages, installing);
		}
	}

	std::string install_script;
	if (options.pluscommands) {
		const std::string script_name = has_member(".PKGINSTALL") ? ".PKGINSTALL" :
			(has_member(".INSTALL") ? ".INSTALL" : "");
		if (script_name.empty()) {
			throw std::runtime_error("pluscommands=yes, but .PKGINSTALL is missing from " + package.name);
		}
		install_script = archive_metadata(archive, script_name);
		if (install_script.empty()) throw std::runtime_error("Could not read " + script_name + " from " + package.name + ".");
	}

	fs::create_directories(directory);
	std::vector<std::string> extract_args{"bsdtar", "-xf", archive.string(), "-C", fs::absolute(directory).string(),
										  "--no-same-owner", "--no-same-permissions", "--safe-writes"};
	for (const auto& metadata : excluded_metadata) extract_args.push_back("--exclude=" + metadata);
	extract_args[1] = "-xvf";
	extract_with_progress(extract_args, members.size(), package.name);

	const auto temporary_manifest = manifest.string() + ".tmp";
	{
		std::ofstream output(temporary_manifest);
		if (!output) throw std::runtime_error("Could not write the package manifest.");
		for (const auto& member : members) output << member << '\n';
		if (!output) throw std::runtime_error("Failed while writing the package manifest.");
	}
	fs::rename(temporary_manifest, manifest);
	std::cout << "Installed " << package.name << " to " << fs::absolute(directory) << '\n';
	if (options.pluscommands) run_install_script(install_script, package.name);
}

}

void install_package(const Package& package, const std::string& repo, const fs::path& directory) {
	const auto manifest = package_database(directory) / (package.name + ".list");
	if (fs::exists(manifest)) throw std::runtime_error("Package is already installed: " + package.name);
	const auto repository_packages = read_packages(repo);
	std::set<std::string> installing;
	install_package_recursive(package, repo, directory, repository_packages, installing);
}

void remove_package(const std::string& name, const fs::path& directory) {
	if (!valid_package_name(name)) throw std::runtime_error("Invalid package name: " + name);
	const auto manifest = package_database(directory) / (name + ".list");
	std::ifstream input(manifest);
	if (!input) throw std::runtime_error("Package is not installed: " + name);

	{
		Activity activity("Removing " + name);
		std::vector<std::string> members;
		std::string member;
		while (std::getline(input, member)) members.push_back(member);
		for (auto it = members.rbegin(); it != members.rend(); ++it) {
			const auto safe = safe_member_path(*it);
			if (!safe.empty()) fs::remove(directory / safe);
		}
		fs::remove(manifest);
		activity.finish();
	}
	std::cout << "Removed " << name << '\n';
}

}
