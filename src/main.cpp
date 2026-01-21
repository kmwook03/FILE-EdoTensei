#include <iostream>
#include "carver.hpp"

int main(int argc, char* argv[]) {
    // check for correct number of arguments
    if (argc != 2) {
        std::cout << "Usage: " << argv[0] << " <disk_image_path>" << std::endl;
        std::cout << "Example: " << argv[0] << " disk.img" << std::endl;
        return 1;
    }

    std::string imagePath = argv[1];
    FileCarver carver(imagePath);

    std::cout << "[*] Initializing File Carver for: " << imagePath << "..." << std::endl;
    if (!carver.initialize()) {
        return 1;
    }

    std::cout << "[*] Starting file carving process. This may take a while..." << std::endl;
    carver.startCarving();

    std::cout << "[*] File carving completed." << std::endl;
    return 0;
}
