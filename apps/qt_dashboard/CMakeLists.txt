cmake_minimum_required(VERSION 3.16)
project(qt_dashboard LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Quick)

qt_add_executable(qt_dashboard main.cpp)

target_link_libraries(qt_dashboard PRIVATE
    Qt6::Core
    Qt6::Quick
)

# Embed QML files into the binary
qt_add_qml_module(qt_dashboard
    URI Dashboard
    VERSION 1.0
    QML_FILES
        Main.qml
        GaugeArc.qml
)
