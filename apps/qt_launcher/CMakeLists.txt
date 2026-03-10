cmake_minimum_required(VERSION 3.16)
project(qt_launcher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Quick)

qt_add_executable(qt_launcher main.cpp)

target_link_libraries(qt_launcher PRIVATE
    Qt6::Core
    Qt6::Quick
)

# Embed QML files into the binary
qt_add_qml_module(qt_launcher
    URI Launcher
    VERSION 1.0
    QML_FILES
        Main.qml
        AppButton.qml
)
