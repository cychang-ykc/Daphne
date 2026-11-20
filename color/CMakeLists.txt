# daphne_color: Tube Color (doc/N28), the effect stage after the upsampler. M1: the stage skeleton; M2 brings the
# curves of the four presets from the SPICE reference data (verify/data/ref).
add_library(daphne_color STATIC
    src/tube_color.cpp src/linear.cpp src/oversampler.cpp
)
target_include_directories(daphne_color PUBLIC include)
target_link_libraries(daphne_color PUBLIC daphne_dsp)
if(DAPHNE_COLOR_TEST_CURVE)
    # test builds only: the polynomial test curve makes the stage usable before M2 (never a release)
    target_compile_definitions(daphne_color PUBLIC DAPHNE_COLOR_TEST_CURVE=1)
endif()
daphne_warnings(daphne_color)
