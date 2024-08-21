add_library(video "video.c")
target_link_libraries(video PkgConfig::LIIBAVCODEC PkgConfig::LIBSWSCALE)

