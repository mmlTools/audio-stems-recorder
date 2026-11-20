# CMake Windows defaults module

include_guard(GLOBAL)

# Enable find_package targets to become globally available targets
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)

include(buildspec)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  # OBS 33+ system-wide plugin root: C:\ProgramData\obs-studio\plugins
  # (converted to forward slashes: a raw "C:\ProgramData" breaks cmake --install)
  file(TO_CMAKE_PATH "$ENV{ALLUSERSPROFILE}/obs-studio/plugins" _obs_plugins_root)
  set(CMAKE_INSTALL_PREFIX "${_obs_plugins_root}" CACHE STRING "Default plugin installation directory" FORCE)
endif()
