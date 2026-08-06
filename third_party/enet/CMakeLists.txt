add_library(enet STATIC
    callbacks.c
    compress.c
    host.c
    list.c
    packet.c
    peer.c
    protocol.c
    unix.c
    win32.c)

target_include_directories(enet PUBLIC include)

if(WIN32)
    target_link_libraries(enet PUBLIC winmm ws2_32)
endif()

if(MSVC)
    target_compile_definitions(enet PRIVATE _WINSOCK_DEPRECATED_NO_WARNINGS)
    # C5287: enum-command/flag bitwise mix (pre-existing in upstream ENet). C4996: winsock deprecation.
    target_compile_options(enet PRIVATE /W3 /wd5287 /wd4996)
endif()