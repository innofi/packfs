set(srcs "src/packfs.c" "src/fileops.c" "src/statops.c" "src/dirops.c")
set(requires "mbedtls")

# Add LZO files
if(CONFIG_PACKFS_LZO_SUPPORT)
    list(APPEND srcs "src/lzoops.c" "src/minilzo.c")
endif()

# Add Process files
if(CONFIG_PACKFS_PROCESS_SUPPORT)
    list(APPEND srcs "src/process.c")
endif()

# Add Stream files
if(CONFIG_PACKFS_STREAM_SUPPORT)
    list(APPEND srcs "src/stream.c")
endif()

# Add ImageFS files
if(CONFIG_IMAGEFS_SUPPORT)
    list(APPEND srcs "src/imagefs.c" "src/imagefsops.c")
endif()

# Add ImageFS DFU
if(CONFIG_IMAGEFS_DFU_SUPPORT)
    list(APPEND srcs "src/imagefsdfu.c")
    list(APPEND requires app_update)
endif()


idf_component_register(SRCS "${srcs}"
    INCLUDE_DIRS "src" "include"
    REQUIRES ${requires})
