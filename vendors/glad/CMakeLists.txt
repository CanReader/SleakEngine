# Define glad as its own static library
add_library(glad STATIC 
    src/glad.c
)

# Tell anyone who uses 'glad' where the headers are
target_include_directories(glad PUBLIC 
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# Link against OpenGL (required for glad)
find_package(OpenGL REQUIRED)
target_link_libraries(glad PRIVATE OpenGL::GL)
