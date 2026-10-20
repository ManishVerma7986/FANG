#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>

int main() {
    std::string path = "A:/PYTHON PROJECT/Fang root folder/Fang/out/staging/guest_manifest.json";
    std::cout << "Path: " << path << std::endl;
    std::cout << "Exists: " << std::filesystem::exists(path) << std::endl;
    
    std::ifstream file(path);
    if (!file) {
        std::cout << "Failed to open file" << std::endl;
        return 1;
    }
    
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string json = ss.str();
    
    std::cout << "JSON length: " << json.length() << std::endl;
    std::cout << "First char: " << (json.empty() ? "EMPTY" : std::string(1, json[0])) << std::endl;
    std::cout << "First 50 chars: " << json.substr(0, 50) << std::endl;
    
    return 0;
}