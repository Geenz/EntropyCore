#include <gtest/gtest.h>
#include <iostream>

TEST(Minimal, Test) {
    SUCCEED();
}

// Provide our own main instead of gtest_main
int main(int argc, char** argv) {
    std::cout << "Starting main..." << std::endl;
    ::testing::InitGoogleTest(&argc, argv);
    std::cout << "GTest initialized..." << std::endl;
    int result = RUN_ALL_TESTS();
    std::cout << "Tests complete, result: " << result << std::endl;
    return result;
}
