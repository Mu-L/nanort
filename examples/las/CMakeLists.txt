cmake_minimum_required(VERSION 3.16)

project(${TARGET} CXX)

set(TARGET lasrender)

option(LASRENDER_USE_PDAL "Use PDAL instead of libLAS" ON)

find_package(OpenGL REQUIRED)
find_package(GLEW REQUIRED)

set(SOURCES 
   main.cc
   render.cc
   render-config.cc
   ${PROJECT_SOURCE_DIR}/../common/trackball.cc
   ${PROJECT_SOURCE_DIR}/../common/matrix.cc
   ${PROJECT_SOURCE_DIR}/../common/imgui/imgui.cpp
   ${PROJECT_SOURCE_DIR}/../common/imgui/imgui_draw.cpp
   ${PROJECT_SOURCE_DIR}/../common/imgui/imgui_widgets.cpp
   ${PROJECT_SOURCE_DIR}/../common/imgui/imgui_impl_btgui.cpp
  )

if (UNIX)
  set(UI_SOURCES
      ${PROJECT_SOURCE_DIR}/../common/OpenGLWindow/X11OpenGLWindow.cpp
      ${PROJECT_SOURCE_DIR}/../common/OpenGLWindow/X11OpenGLWindow.h
      )
endif()

if (LASRENDER_USE_PDAL)
  find_package(PDAL REQUIRED CONFIG)
  set(DEP_LIBS ${PDAL_LIBRARIES})
else()
  find_package(las REQUIRED)
  set(DEP_LIBS las)
endif()

add_executable(${TARGET} ${SOURCES} ${UI_SOURCES})
if (UNIX)
  target_link_libraries(${TARGET} X11)
endif()
target_link_libraries(${TARGET} ${DEP_LIBS} GL ${GLEW_LIBRARIES} ${CMAKE_DL_LIBS})
target_include_directories(${TARGET} PRIVATE ${PROJECT_SOURCE_DIR}/../common/)
target_include_directories(${TARGET} PRIVATE ${PROJECT_SOURCE_DIR}/../common/imgui)
target_include_directories(${TARGET} PRIVATE ${GLEW_INCLUDE_DIRS})

if (LASRENDER_USE_PDAL)
  target_include_directories(${TARGET} PRIVATE ${PDAL_INCLUDE_DIRS})
  target_link_directories(${TARGET} PRIVATE ${PDAL_LIBRARY_DIRS})
  target_compile_definitions(${TARGET} PRIVATE LASRENDER_USE_PDAL)
endif()
