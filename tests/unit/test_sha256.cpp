#include "canonical_encoder.hpp"
#include <iostream>
#include <iomanip>

int main() {
    using namespace fang::studio::canonical;
    
    // Test SHA-256 of empty string
    std::vector<uint8_t> empty;
    std::string hash = sha256_hex(empty);
    std::cout << "SHA-256 of empty string: " << hash << std::endl;
    std::cout << "Expected: e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" << std::endl;
    std::cout << "Match: " << (hash == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" ? "YES" : "NO") << std::endl;
    
    // Test SHA-256 of "abc"
    std::vector<uint8_t> abc = {'a', 'b', 'c'};
    hash = sha256_hex(abc);
    std::cout << "SHA-256 of 'abc': " << hash << std::endl;
    std::cout << "Expected: ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" << std::endl;
    std::cout << "Match: " << (hash == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" ? "YES" : "NO") << std::endl;
    
    // Test with the canonical bytes from single_file_entry
    // (first 100 bytes for brevity)
    std::vector<uint8_t> test_bytes;
    // Magic: FANGCIM\0\1
    test_bytes.insert(test_bytes.end(), {'F','A','N','G','C','I','M', 0x00, 0x01});
    // Scope length (27) + scope
    test_bytes.push_back(0x00); test_bytes.push_back(0x00); test_bytes.push_back(0x00); test_bytes.push_back(0x1b);
    std::string scope = "fang-guest-daemon-inputs-v1";
    test_bytes.insert(test_bytes.end(), scope.begin(), scope.end());
    
    hash = sha256_hex(test_bytes);
    std::cout << "SHA-256 of partial canonical: " << hash << std::endl;
    
    return 0;
}