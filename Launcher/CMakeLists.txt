add_executable(KeeperForge2Launcher)
target_sources(KeeperForge2Launcher PRIVATE
	Source/main.cpp
	Source/FolderLayout.cpp
	Source/GameFiles.cpp
	Source/Launcher.cpp
	Source/PEUtils.cpp
	Resources/Launcher.rc
)
# So a new icon rebuilds the resource.
set_source_files_properties(Resources/Launcher.rc PROPERTIES
	OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Resources/KeeperForge2Launcher.ico"
)
target_link_libraries(KeeperForge2Launcher PRIVATE
	KFShared
	bcrypt
)
# DK2 runs elevated, so the launcher must too (UAC prompt on start)
if(MSVC)
	target_link_options(KeeperForge2Launcher PRIVATE
		"/MANIFESTUAC:level='requireAdministrator'"
	)
endif()
target_include_directories(KeeperForge2Launcher PUBLIC
	Include
	../Shared/Include
)
