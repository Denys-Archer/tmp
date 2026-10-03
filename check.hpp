#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace bavovna {

struct Package {
	std::string name;
	std::string archive;
};

class Activity {
public:
	explicit Activity(std::string message);
	~Activity();
	Activity(const Activity&) = delete;
	Activity& operator=(const Activity&) = delete;
	void finish(bool success = true);

private:
	struct State;
	std::unique_ptr<State> state_;
};

std::string lowercase(std::string value);
std::string read_config(const std::filesystem::path& config_path);
std::string fetch_text(const std::string& url);
std::vector<Package> read_packages(const std::string& repo);
const Package& find_package(const std::vector<Package>& packages, const std::string& query);
std::vector<Package> search_packages(const std::vector<Package>& packages, const std::string& query);
void install_package(const Package& package, const std::string& repo, const std::filesystem::path& directory);
void remove_package(const std::string& name, const std::filesystem::path& directory);

}
