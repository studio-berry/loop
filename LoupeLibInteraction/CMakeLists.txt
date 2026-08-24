add_library(LoupeLibInteraction STATIC
    sources/preflightfindingsmodel.cpp
    sources/preflightfindingsmodel.h
    sources/preflightcontroller.cpp
    sources/preflightcontroller.h
)

target_link_libraries(LoupeLibInteraction
    PUBLIC LoupeLibCore Qt6::Core Qt6::Gui
)

target_include_directories(LoupeLibInteraction
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/sources
)

set_target_properties(LoupeLibInteraction PROPERTIES
    AUTOMOC ON
    AUTOUIC OFF
    AUTORCC OFF
)
