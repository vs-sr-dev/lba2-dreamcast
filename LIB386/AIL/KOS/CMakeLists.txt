set(AIL_KOS_SOURCES
    SOUND_KOS_BACKEND.CPP
    STREAM_KOS.CPP
    # stb_vorbis.c lives in ../SDL/ — it's pure C with no SDL dependency,
    # we just reuse the same source file from there for the KOS backend.
    ${CMAKE_CURRENT_SOURCE_DIR}/../SDL/stb_vorbis.c
)
if(MVIDEO_BACKEND STREQUAL "smacker")
    list(APPEND AIL_KOS_SOURCES VIDEO_AUDIO_KOS.CPP)
endif()
target_sources(ail PRIVATE ${AIL_KOS_SOURCES})
target_compile_definitions(ail PUBLIC USE_SOUND_KOS)
target_include_directories(ail PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../SDL)
# stb_vorbis is plain C; tame benign warnings from its file-scope statics so
# the KOS toolchain (-Werror? no, but -Wall is on) doesn't drown the log.
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/../SDL/stb_vorbis.c
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-but-set-variable;-Wno-unused-value")
# KOS audio symbols (snd_init, snd_sfx_*, snd_stream_*) live in libkallisti,
# linked into every Dreamcast executable by default — no extra link line.
