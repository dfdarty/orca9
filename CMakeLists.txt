# Orca-9: Pod Commander — builds for the emulator and, with the Pico SDK and
# Arm GCC, into a UF2 for the FREE-WILi 2 (fw2emu hwcheck).
add_executable(orca9 main.c render.c meshes.c)
target_link_libraries(orca9 freewili2_bsp)
fw2_display_app(orca9
    POWER_ZONES DISPLAY SENSORS
    VERSION 001
    DESCRIPTION "Orca-9: Pod Commander - 3D space shooter, steered by tilting")
