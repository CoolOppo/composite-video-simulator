cmake_minimum_required (VERSION 3.13.4)
project (composite-video-simulator LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native -Wno-deprecated-declarations -fopenmp")
set(CMAKE_CXX_FLAGS_RELEASE "-DNDEBUG -g0 -Ofast -flto")
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR})

find_package(FFMPEG COMPONENTS avformat avcodec avutil avdevice swscale swresample REQUIRED)
find_package(Boost COMPONENTS fiber context REQUIRED)

add_executable (ffmpeg_average_delay ffmpeg_average_delay.cpp)
target_link_libraries(ffmpeg_average_delay ${FFMPEG_LIBRARIES})
target_include_directories(ffmpeg_average_delay PUBLIC ${FFMPEG_INCLUDE_DIRS})

add_executable (ffmpeg_cassette ffmpeg_cassette.cpp)
target_link_libraries(ffmpeg_cassette ${FFMPEG_LIBRARIES})
target_include_directories(ffmpeg_cassette PUBLIC ${FFMPEG_INCLUDE_DIRS})


add_executable (ffmpeg_colorkey ffmpeg_colorkey.cpp)
target_link_libraries(ffmpeg_colorkey ${FFMPEG_LIBRARIES})
target_include_directories(ffmpeg_colorkey PUBLIC ${FFMPEG_INCLUDE_DIRS})


add_executable (ffmpeg_colormap ffmpeg_colormap.cpp)
target_link_libraries(ffmpeg_colormap ${FFMPEG_LIBRARIES})
target_include_directories(ffmpeg_colormap PUBLIC ${FFMPEG_INCLUDE_DIRS})

add_executable (ffmpeg_ntsc ffmpeg_ntsc.cpp)
target_link_libraries(ffmpeg_ntsc ${FFMPEG_LIBRARIES} ${Boost_LIBRARIES})
target_include_directories(ffmpeg_ntsc PUBLIC ${FFMPEG_INCLUDE_DIRS} ${Boost_INCLUDE_DIRS})

add_executable (ffmpeg_posterize ffmpeg_posterize.cpp)
target_link_libraries(ffmpeg_posterize ${FFMPEG_LIBRARIES})
target_include_directories(ffmpeg_posterize PUBLIC ${FFMPEG_INCLUDE_DIRS})

add_executable (ffmpeg_scanimate ffmpeg_scanimate.cpp)
target_link_libraries(ffmpeg_scanimate ${FFMPEG_LIBRARIES})
target_include_directories(ffmpeg_scanimate PUBLIC ${FFMPEG_INCLUDE_DIRS})

add_executable (ffmpeg_to_composite ffmpeg_to_composite.cpp)
target_link_libraries(ffmpeg_to_composite ${FFMPEG_LIBRARIES})
target_include_directories(ffmpeg_to_composite PUBLIC ${FFMPEG_INCLUDE_DIRS})

add_executable (frameblend frameblend.cpp)
target_link_libraries(frameblend ${FFMPEG_LIBRARIES})
target_include_directories(frameblend PUBLIC ${FFMPEG_INCLUDE_DIRS})

add_executable (normalize_ts normalize_ts.cpp)
target_link_libraries(normalize_ts ${FFMPEG_LIBRARIES})
target_include_directories(normalize_ts PUBLIC ${FFMPEG_INCLUDE_DIRS})
