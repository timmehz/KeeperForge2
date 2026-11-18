cmake_minimum_required(VERSION 3.20)
project(
	KeeperForge2
	VERSION 0.1.0
	LANGUAGES CXX RC
)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# For MSVC, CMake turns C++23 into /std:c++latest, which moves on to newer drafts with compiler updates. Ask for C++23.
if(MSVC AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 19.43)
	set(CMAKE_CXX23_STANDARD_COMPILE_OPTION "-std:c++23preview")
	set(CMAKE_CXX23_EXTENSION_COMPILE_OPTION "-std:c++23preview")
endif()

add_subdirectory(Shared)
add_subdirectory(Runtime)
add_subdirectory(Launcher)
