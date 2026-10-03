#pragma once

#include <filesystem>
#include <string>

namespace bavovna::downloader {

std::string get(const std::string& url);
void download(const std::string& url, const std::filesystem::path& destination,
			  const std::string& progress_label);

}
