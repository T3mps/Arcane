#include <cstring>
#include <gtest/gtest.h>
#include "Astra/Container/AlignedStorage.hpp"

class AlignedStorageTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Test basic type storage
TEST_F(AlignedStorageTest, BasicTypeStorage)
{
    Astra::AlignedStorage<int, double> storage;
    
    // Store an int
    *storage.As<int>() = 42;
    EXPECT_EQ(*storage.As<int>(), 42);
    
    // Store a double (overwrites int)
    *storage.As<double>() = 3.14159;
    EXPECT_DOUBLE_EQ(*storage.As<double>(), 3.14159);
}

// Test const access
TEST_F(AlignedStorageTest, ConstAccess)
{
    Astra::AlignedStorage<int, float> storage;
    *storage.As<int>() = 100;
    
    const auto& constStorage = storage;
    EXPECT_EQ(*constStorage.As<int>(), 100);
}

// Test alignment requirements
TEST_F(AlignedStorageTest, AlignmentRequirements)
{
    struct Aligned16
    {
        alignas(16) char data[16];
    };
    
    struct Aligned8
    {
        alignas(8) char data[8];
    };
    
    using Storage = Astra::AlignedStorage<Aligned16, Aligned8>;
    
    // Check size is max of the two
    EXPECT_EQ(sizeof(Storage::data), std::max(sizeof(Aligned16), sizeof(Aligned8)));
    
    // Check alignment is max of the two
    EXPECT_EQ(Storage::alignment, std::max(alignof(Aligned16), alignof(Aligned8)));
    EXPECT_EQ(Storage::alignment, 16u);
    
    // Verify actual alignment of storage instance
    Storage storage;
    EXPECT_EQ(reinterpret_cast<uintptr_t>(&storage.data) % 16, 0u);
}

// Test with complex types
TEST_F(AlignedStorageTest, ComplexTypes)
{
    struct SmallStruct
    {
        int a;
        char b;
        // Size: 8 bytes (with padding)
    };
    
    struct LargeStruct
    {
        double values[4];
        int count;
        // Size: 40 bytes (32 + 4 + padding)
    };
    
    Astra::AlignedStorage<SmallStruct, LargeStruct> storage;
    
    // Store SmallStruct
    storage.As<SmallStruct>()->a = 123;
    storage.As<SmallStruct>()->b = 'X';
    
    EXPECT_EQ(storage.As<SmallStruct>()->a, 123);
    EXPECT_EQ(storage.As<SmallStruct>()->b, 'X');
    
    // Store LargeStruct
    auto* large = storage.As<LargeStruct>();
    large->values[0] = 1.0;
    large->values[1] = 2.0;
    large->values[2] = 3.0;
    large->values[3] = 4.0;
    large->count = 4;
    
    EXPECT_DOUBLE_EQ(storage.As<LargeStruct>()->values[0], 1.0);
    EXPECT_DOUBLE_EQ(storage.As<LargeStruct>()->values[3], 4.0);
    EXPECT_EQ(storage.As<LargeStruct>()->count, 4);
}

// Test void specialization
TEST_F(AlignedStorageTest, VoidSpecialization)
{
    struct ErrorType
    {
        int code;
        char message[64];
    };
    
    Astra::AlignedStorage<void, ErrorType> storage;
    
    // Can only store ErrorType
    storage.As<ErrorType>()->code = 404;
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
    std::strcpy(storage.As<ErrorType>()->message, "Not Found");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    
    EXPECT_EQ(storage.As<ErrorType>()->code, 404);
    EXPECT_STREQ(storage.As<ErrorType>()->message, "Not Found");
    
    // Verify size and alignment
    EXPECT_EQ(sizeof(storage.data), sizeof(ErrorType));
    EXPECT_EQ(alignof(decltype(storage)), alignof(ErrorType));
}

// Test that storage preserves bytes when switching types
TEST_F(AlignedStorageTest, BytePreservation)
{
    Astra::AlignedStorage<uint32_t, float> storage;
    
    // Store a specific bit pattern as uint32_t
    *storage.As<uint32_t>() = 0x3F800000; // IEEE 754 representation of 1.0f
    
    // Read as float - should be 1.0
    EXPECT_EQ(*storage.As<float>(), 1.0f);
    
    // Store as float
    *storage.As<float>() = 2.0f;
    
    // Read as uint32_t - should be IEEE 754 representation of 2.0f
    EXPECT_EQ(*storage.As<uint32_t>(), 0x40000000u);
}

// Test with arrays
TEST_F(AlignedStorageTest, ArrayTypes)
{
    Astra::AlignedStorage<int[4], double[2]> storage;
    
    // Store int array
    auto* intArray = storage.As<int[4]>();
    (*intArray)[0] = 10;
    (*intArray)[1] = 20;
    (*intArray)[2] = 30;
    (*intArray)[3] = 40;
    
    EXPECT_EQ((*storage.As<int[4]>())[0], 10);
    EXPECT_EQ((*storage.As<int[4]>())[3], 40);
    
    // Store double array
    auto* doubleArray = storage.As<double[2]>();
    (*doubleArray)[0] = 1.5;
    (*doubleArray)[1] = 2.5;
    
    EXPECT_DOUBLE_EQ((*storage.As<double[2]>())[0], 1.5);
    EXPECT_DOUBLE_EQ((*storage.As<double[2]>())[1], 2.5);
}

// Test size calculations
TEST_F(AlignedStorageTest, SizeCalculations)
{
    // Test with types of different sizes
    {
        using Storage1 = Astra::AlignedStorage<char, int>;
        EXPECT_EQ(Storage1::size, sizeof(int));
        EXPECT_EQ(Storage1::alignment, alignof(int));
    }
    
    {
        using Storage2 = Astra::AlignedStorage<double, char>;
        EXPECT_EQ(Storage2::size, sizeof(double));
        EXPECT_EQ(Storage2::alignment, alignof(double));
    }
    
    {
        struct Large { char data[100]; };
        struct Small { char data[10]; };
        using Storage3 = Astra::AlignedStorage<Large, Small>;
        EXPECT_EQ(Storage3::size, sizeof(Large));
    }
}