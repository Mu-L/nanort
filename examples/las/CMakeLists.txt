cmake_minimum_required(VERSION 3.16)

project(${TARGET} CXX)

set(TARGET lasrender)

option(LASRENDER_USE_PDAL "Use PDAL instead of libLAS" ON)

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
  set(DEP_LIBS pdal)
else()
  find_package(las REQUIRED)
  set(DEP_LIBS las)
endif()

add_executable(${TARGET} ${SOURCES} ${UI_SOURCES})
target_link_libraries(${TARGET} ${DEP_LIBS})
target_include_directories(${TARGET} PRIVATE ${PROJECT_SOURCE_DIR}/../common/)
target_include_directories(${TARGET} PRIVATE ${PROJECT_SOURCE_DIR}/../common/imgui)

if (LASRENDER_USE_PDAL)
  target_compile_definitions(${TARGET} PRIVATE LASRENDER_USE_PDAL)
endif()
