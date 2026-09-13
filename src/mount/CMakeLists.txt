add_library(wsldrive_mount STATIC fuse_mount.cpp fsnotify_bridge.cpp)
add_library(wsldrive::mount ALIAS wsldrive_mount)

# WSLDRIVE_HAVE_MOUNT is PUBLIC so the CLI enables the mount subcommand on either
# platform; the WinFsp target additionally defines WSLDRIVE_HAVE_WINFSP on Windows.
target_compile_definitions(wsldrive_mount PUBLIC WSLDRIVE_HAVE_MOUNT)
target_link_libraries(wsldrive_mount PUBLIC wsldrive::agent PRIVATE wsldrive::options)

if(WIN32)
  target_link_libraries(wsldrive_mount PUBLIC winfsp::fuse3 PRIVATE advapi32)
else()
  target_link_libraries(wsldrive_mount PUBLIC wsldrive::fuse3)
endif()
