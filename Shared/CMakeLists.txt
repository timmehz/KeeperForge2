add_library(KFShared STATIC)
target_sources(
	KFShared
	PRIVATE
		Source/Config.cpp
		Source/ConfigFile.cpp
		Source/DisplayUtils.cpp
		Source/Logger.cpp
		Source/Paths.cpp
		Source/StringUtils.cpp
)
target_include_directories(
	KFShared
	PUBLIC
		Include
)
