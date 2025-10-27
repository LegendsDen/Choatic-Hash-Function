// main.cpp
#include <iostream>
#include <string>
#include <vector>
#include "HybridParallelHash.hpp" // Include your header

int main() {
    try {
        // 1. Create an instance of the hash class.
        // You can use the default parameters:
        HybridParallelHash hasher;

        // Or specify your own:
        // HybridParallelHash hasher(3.8, 2.7, 1200, 800);

        // 2. Define some test messages
        std::string msg1 = "Hello, world!";
        std::string msg2 = "Hello, world."; // Tiny change (period)

        // 3. Compute and print the hashes
        std::cout << "--- Hybrid Chaotic Hash ---" << std::endl;
        
        std::cout << "\nMessage 1: \"" << msg1 << "\"" << std::endl;
        std::cout << "  SHA-128: " << hasher.digest(msg1, 128) << std::endl;
        std::cout << "  SHA-256: " << hasher.digest(msg1, 256) << std::endl;
        std::cout << "  SHA-512: " << hasher.digest(msg1, 512) << std::endl;

        std::cout << "\nMessage 2: \"" << msg2 << "\"" << std::endl;
        std::cout << "  SHA-256: " << hasher.digest(msg2, 256) << std::endl;
        
        std::cout << "\nDemonstrating the avalanche effect." << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "An error occurred: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}