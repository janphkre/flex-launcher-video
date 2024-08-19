add_library(video "video.c" "theoraplay.c")
target_link_libraries(video PkgConfig::SDL2 PkgConfig::OGG PkgConfig::VORBIS PkgConfig::THEORADEC)

