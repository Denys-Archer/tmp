#include "check.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

void print_usage(const char* executable) {
	std::cout << "Usage: " << executable
			  << " [--config FILE] [-dir DIR] COMMAND [ARG]\n"
			  << "Commands: list, search QUERY, info NAME, news, install NAME, remove NAME\n"
			  << "System privileges are required for news, install, and remove.\n";
}

void print_version() {
	std::cout << "Bavovna package manager 0.1 version\n"
			  << "License: GNU General Public License v3.0 (GPL-3.0)\n"
			  << "A package manager for Bavovna repositories.\n"
			  << "This is free software; there is no warranty.\n";
}

void require_superuser(const std::string& command) {
	if (geteuid() != 0) {
		throw std::runtime_error("The '" + command + "' command requires superuser privileges. Run it with sudo or as root.");
	}
}

bool is_config_file(const fs::path& path) {
	std::error_code error;
	return fs::is_regular_file(path, error) && !error && path.filename() == "bavovna-config.txt";
}

fs::path find_config(const char* executable) {
	std::vector<fs::path> candidates{
		"/etc/bavovna-config.txt",
		fs::current_path() / "bavovna-config.txt",
		fs::absolute(executable).parent_path() / "bavovna-config.txt"
	};
	for (const auto& candidate : candidates) {
		if (is_config_file(candidate)) return candidate;
	}

	const fs::directory_options options = fs::directory_options::skip_permission_denied;
	std::error_code error;
	fs::recursive_directory_iterator current("/", options, error);
	const fs::recursive_directory_iterator end;
	while (!error && current != end) {
		const auto path = current->path();
		if (current->is_directory(error)) {
			const auto normalized = path.lexically_normal().generic_string();
			if (normalized == "/proc" || normalized == "/sys" || normalized == "/dev" ||
				normalized == "/run" || normalized == "/tmp") {
				current.disable_recursion_pending();
			}
		}
		if (is_config_file(path)) return path;
		current.increment(error);
		if (error == std::errc::permission_denied || error == std::errc::no_such_file_or_directory) error.clear();
	}
	throw std::runtime_error("Could not find bavovna-config.txt. Use --config FILE to specify its location.");
}

}

int main(int argc, char* argv[]) {
	try {
		fs::path config_path;
		bool config_was_set = false;
		fs::path directory = "/";
		std::vector<std::string> positional;
		for (int i = 1; i < argc; ++i) {
			const std::string arg = argv[i];
			if ((arg == "--config" || arg == "--root" || arg == "-dir") && i + 1 < argc) {
				if (arg == "--config") {
					config_path = argv[++i];
					config_was_set = true;
				}
				else directory = argv[++i];
			} else if (arg == "--help" || arg == "-h") {
				print_usage(argv[0]);
				return 0;
			} else if (arg == "--version" || arg == "-V") {
				print_version();
				return 0;
			} else {
				positional.push_back(arg);
			}
		}
		if (positional.empty()) {
			print_usage(argv[0]);
			return 2;
		}

		const auto command = bavovna::lowercase(positional[0]);
		if (command == "remove") {
			if (positional.size() != 2) throw std::runtime_error("Specify a package name to remove.");
			require_superuser(command);
			bavovna::remove_package(positional[1], directory);
			return 0;
		}
		if (command == "install") {
			if (positional.size() != 2) throw std::runtime_error("Specify a package name to install.");
			require_superuser(command);
		} else if (command == "news") {
			require_superuser(command);
		}

		if (!config_was_set) config_path = find_config(argv[0]);
		const auto repo = bavovna::read_config(config_path);
		if (command == "news") {
			std::cout << bavovna::fetch_text(repo + "/news.txt");
			return 0;
		}

		const auto packages = bavovna::read_packages(repo);
		if (command == "list") {
			for (const auto& package : packages) std::cout << package.name << "\t" << package.archive << '\n';
		} else if (command == "search") {
			if (positional.size() != 2) throw std::runtime_error("Specify a search query.");
			for (const auto& package : bavovna::search_packages(packages, positional[1])) {
				std::cout << package.name << "\t" << package.archive << '\n';
			}
		} else if (command == "info") {
			if (positional.size() != 2) throw std::runtime_error("Specify a package name.");
			const auto& package = bavovna::find_package(packages, positional[1]);
			std::cout << "Package: " << package.name << "\nArchive: " << package.archive
					  << "\nRepository: " << repo << '\n';
		} else if (command == "install") {
			bavovna::install_package(bavovna::find_package(packages, positional[1]), repo, directory);
		} else {
			print_usage(argv[0]);
			return 2;
		}
	} catch (const std::exception& error) {
		std::cerr << "Error: " << error.what() << '\n';
		return 1;
	}
	return 0;
}
