# Native Linux build (docs/LINUX_PORT_PLAN.md), selected with -DX4VR_LINUX=ON from the top-level
# CMakeLists.txt. Dependencies come from the system (Nix); nothing is downloaded.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
include(GNUInstallDirs)
set(X4VR_ROOT ${PROJECT_SOURCE_DIR})

find_path(VULKAN_HEADERS_DIR vulkan/vk_layer.h REQUIRED)
find_path(OPENVR_INCLUDE_DIR openvr.h PATH_SUFFIXES openvr REQUIRED)
find_library(OPENVR_LIBRARY openvr_api REQUIRED)

set(X4VR_WARNINGS -Wall -Wextra -Werror)

# Portable code shared with the Windows build, compiled as it is.
add_library(x4vr_vr STATIC ${X4VR_ROOT}/src/math.cpp ${X4VR_ROOT}/src/session.cpp)
target_include_directories(x4vr_vr PUBLIC ${X4VR_ROOT}/include ${OPENVR_INCLUDE_DIR} ${VULKAN_HEADERS_DIR})
target_compile_definitions(x4vr_vr PUBLIC VK_NO_PROTOTYPES)
target_link_libraries(x4vr_vr PUBLIC ${OPENVR_LIBRARY})

add_library(x4vr_elf STATIC ${X4VR_ROOT}/src/linux/elf_classes.cpp)
target_include_directories(x4vr_elf PUBLIC ${X4VR_ROOT}/src/linux)
target_compile_options(x4vr_elf PRIVATE ${X4VR_WARNINGS})

add_executable(x4vr ${X4VR_ROOT}/tools/linux/x4vr_cli.cpp)
target_link_libraries(x4vr PRIVATE x4vr_vr x4vr_elf)
target_compile_options(x4vr PRIVATE ${X4VR_WARNINGS})

# Libraries loaded into X4 (and, for the preload, into every process Steam starts on the way):
# own copies of the compiler runtime, only their entry points exported, no unresolved symbols.
add_library(x4vr_probe SHARED ${X4VR_ROOT}/src/linux/probe_preload.c)
set_target_properties(x4vr_probe PROPERTIES C_VISIBILITY_PRESET hidden)
target_compile_options(x4vr_probe PRIVATE ${X4VR_WARNINGS})
target_link_options(x4vr_probe PRIVATE -Wl,--exclude-libs,ALL -Wl,-z,defs)

add_library(VkLayer_x4vr_probe SHARED ${X4VR_ROOT}/src/linux/probe_layer.cpp)
target_include_directories(VkLayer_x4vr_probe PRIVATE ${VULKAN_HEADERS_DIR})
target_compile_definitions(VkLayer_x4vr_probe PRIVATE VK_NO_PROTOTYPES)
set_target_properties(VkLayer_x4vr_probe PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_compile_options(VkLayer_x4vr_probe PRIVATE ${X4VR_WARNINGS})
target_link_options(VkLayer_x4vr_probe PRIVATE -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL -Wl,-z,defs)

# Layer manifests: one for the installed package, one for tests in the build tree.
set(X4VR_PROBE_LAYER_PATH ${CMAKE_INSTALL_FULL_LIBDIR}/libVkLayer_x4vr_probe.so)
configure_file(VkLayer_x4vr_probe.json.in ${CMAKE_CURRENT_BINARY_DIR}/install/VkLayer_x4vr_probe.json @ONLY)
set(X4VR_PROBE_LAYER_PATH ${CMAKE_CURRENT_BINARY_DIR}/libVkLayer_x4vr_probe.so)
configure_file(VkLayer_x4vr_probe.json.in ${CMAKE_CURRENT_BINARY_DIR}/layer/VkLayer_x4vr_probe.json @ONLY)
configure_file(x4vr-probe-run.in ${CMAKE_CURRENT_BINARY_DIR}/x4vr-probe-run @ONLY)

install(TARGETS x4vr RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS x4vr_probe VkLayer_x4vr_probe LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/install/VkLayer_x4vr_probe.json DESTINATION ${CMAKE_INSTALL_DATADIR}/vulkan/explicit_layer.d)
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/x4vr-probe-run DESTINATION ${CMAKE_INSTALL_BINDIR})

if(BUILD_TESTING) # include(CTest) is in the top-level CMakeLists.txt, so ctest finds these
  add_executable(opentrack_tests ${X4VR_ROOT}/tests/linux/opentrack_tests.cpp)
  target_include_directories(opentrack_tests PRIVATE ${X4VR_ROOT}/src/linux)
  target_compile_options(opentrack_tests PRIVATE ${X4VR_WARNINGS})
  add_test(NAME opentrack_packet COMMAND opentrack_tests)

  # The scan reads absolute pointers from the file, as in Linux X4: the test inspects itself, so
  # it is linked without PIE like the game.
  add_executable(elf_classes_tests ${X4VR_ROOT}/tests/linux/elf_classes_tests.cpp)
  target_link_libraries(elf_classes_tests PRIVATE x4vr_elf)
  set_target_properties(elf_classes_tests PROPERTIES POSITION_INDEPENDENT_CODE OFF)
  target_compile_options(elf_classes_tests PRIVATE ${X4VR_WARNINGS} -fno-pie)
  target_link_options(elf_classes_tests PRIVATE -no-pie)
  add_test(NAME elf_classes COMMAND elf_classes_tests)

  add_executable(probe_socket_fixture ${X4VR_ROOT}/tests/linux/probe_socket_fixture.cpp)
  target_include_directories(probe_socket_fixture PRIVATE ${X4VR_ROOT}/src/linux)
  target_compile_options(probe_socket_fixture PRIVATE ${X4VR_WARNINGS})
  add_test(NAME probe_preload COMMAND ${CMAKE_COMMAND} -DFIXTURE=$<TARGET_FILE:probe_socket_fixture>
           -DPROBE=$<TARGET_FILE:x4vr_probe> -DDIR=${CMAKE_CURRENT_BINARY_DIR}/probe-test
           -P ${X4VR_ROOT}/tests/linux/probe_preload_test.cmake)
endif()
