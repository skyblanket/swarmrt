# CMake toolchain file for cross-compiling SwarmRT to iOS (arm64)
set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_ARCHITECTURES arm64)
set(CMAKE_OSX_DEPLOYMENT_TARGET "17.0")

# Use the iPhone SDK
execute_process(
    COMMAND xcrun --sdk iphoneos --show-sdk-path
    OUTPUT_VARIABLE CMAKE_OSX_SYSROOT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Use Xcode's clang
execute_process(
    COMMAND xcrun --sdk iphoneos --find clang
    OUTPUT_VARIABLE CMAKE_C_COMPILER
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# ASM compiler must be the same
set(CMAKE_ASM_COMPILER ${CMAKE_C_COMPILER})

# Don't try to run test executables during configure
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Bitcode is dead, but set flags for proper iOS compilation
set(CMAKE_C_FLAGS_INIT "-fembed-bitcode-marker")
