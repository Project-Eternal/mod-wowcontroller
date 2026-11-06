# mod-wowcontroller's navigation meshes (src/NavTiles.cpp) are made with Recast, which the server itself does not link (only the mmaps
# generator): the module links it.
target_link_libraries(modules PRIVATE Recast)
