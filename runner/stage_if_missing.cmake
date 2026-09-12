# cmake -DSRC=<file> -DDST=<file> -P stage_if_missing.cmake
# Copy SRC to DST unless DST already exists (or SRC does not). Used to seed a
# per-machine file beside the executable without overwriting what the
# runtime later wrote there.
if(EXISTS "${SRC}" AND NOT EXISTS "${DST}")
    get_filename_component(_dir "${DST}" DIRECTORY)
    file(MAKE_DIRECTORY "${_dir}")
    configure_file("${SRC}" "${DST}" COPYONLY)
    message(STATUS "staged ${DST} from ${SRC}")
endif()
