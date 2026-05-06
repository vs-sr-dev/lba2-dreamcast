if(LBA2_TARGET_DREAMCAST)
    # V1: stub the console out entirely. SDL3-coupled CONSOLE.CPP and
    # CONSOLE_STATE.CPP are excluded; CONSOLE_DC_STUB.CPP provides empty impls
    # so the engine link succeeds.
    add_library(console CONSOLE_DC_STUB.CPP)
else()
    add_library(console CONSOLE.CPP CONSOLE_STATE.CPP)
    target_link_libraries(console PRIVATE SDL3::SDL3)
endif()
target_include_directories(console PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_SOURCE_DIR}/LIB386/H
)
target_compile_definitions(console PRIVATE ONE_GAME_DIRECTORY LBA_GAME CDROM SDL_DISABLE_OLD_NAMES)
