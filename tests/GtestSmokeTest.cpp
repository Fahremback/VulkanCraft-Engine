// GtestSmokeTest.cpp — proves googletest integration works
#include <gtest/gtest.h>

TEST(GtestSmoke, BasicArithmetic) {
    EXPECT_EQ(2 + 2, 4);
    EXPECT_NE(3, 4);
    EXPECT_TRUE(1.0 + 1.0 == 2.0);
}

TEST(GtestSmoke, StringComparison) {
    std::string hello = "hello";
    EXPECT_STREQ(hello.c_str(), "hello");
    EXPECT_STRNE(hello.c_str(), "world");
}

TEST(GtestSmoke, ContainerOperations) {
    std::vector<int> v = {1, 2, 3, 4, 5};
    EXPECT_EQ(v.size(), 5u);
    EXPECT_EQ(v[0], 1);
    EXPECT_EQ(v.back(), 5);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
