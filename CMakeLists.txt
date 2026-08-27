cmake_minimum_required(VERSION 3.16)
project(FileSearchFastly LANGUAGES CXX)

include(qt.cmake)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR}
    COMPONENTS
        Quick
        Sql
        Concurrent
)
qt_standard_project_setup()

set(PROJECT_SOURCES
    main.cpp

    app.rc

    HeaderFile/FileScanner.h
    CPPFile/FileScanner.cpp

    HeaderFile/DriveScanner.h
    CPPFile/DriveScanner.cpp

    HeaderFile/FileDatabase.h
    CPPFile/FileDatabase.cpp

    HeaderFile/FileInteract.h
    CPPFile/FileInteract.cpp

    publicHeader/FileInfo.h
)

if(QT_VERSION VERSION_LESS 5.15)
    qt5_add_resources(PROJECT_SOURCES qml.qrc)
elseif(QT_VERSION VERSION_LESS 6.2)
    qt_add_resources(PROJECT_SOURCES qml.qrc)
endif()

qt_add_executable(${PROJECT_NAME} ${PROJECT_SOURCES})

if(QT_VERSION VERSION_GREATER_EQUAL 6.2)
    qt_add_qml_module(${PROJECT_NAME}
        URI FileSearchFastly
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml/"
        QML_FILES
            main.qml
    )
endif()

set_target_properties(${PROJECT_NAME}
    PROPERTIES
        WIN32_EXECUTABLE TRUE
        MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/app.manifest"
)

# 管理员权限 manifest：双击自动提权 → USN 秒级枚举（Everything 同思路）
# Qt6 会覆盖 MANIFEST 属性，用 mt.exe 在链接后强制替换内嵌 manifest
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND mt.exe -nologo
        -manifest "${CMAKE_CURRENT_SOURCE_DIR}/app.manifest"
        "-outputresource:$<TARGET_FILE:${PROJECT_NAME}>;#1"
    COMMENT "Embedding requireAdministrator manifest"
)

target_link_libraries(${PROJECT_NAME}
    PUBLIC
        Qt::Quick
        Qt::Sql
        Qt::Concurrent
)

if(Qt6_FOUND)
    qt_import_qml_plugins(${PROJECT_NAME})
endif()
