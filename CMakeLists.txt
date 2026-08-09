cmake_minimum_required(VERSION 3.16)
project(gxde-dlna-caster VERSION 1.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Qt6 版 DTK2 (libdtk2widget6) + dtk6core / dtk6gui
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network WebSockets LinguistTools)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DTK2 REQUIRED IMPORTED_TARGET dtk2widget dtk6gui dtk6core dtk6log)

set(SOURCES
    main.cpp
    mainwindow.cpp
    dlnascanner.cpp
    avcontrol.cpp
    mediaserver.cpp
    castcontroller.cpp
    browserserver.cpp
)

set(HEADERS
    mainwindow.h
    dlnascanner.h
    avcontrol.h
    mediaserver.h
    castcontroller.h
    browserserver.h
    renderer.h
)

add_executable(gxde-dlna-caster ${SOURCES} ${HEADERS}
    assets/icons/gxde-dlna-caster.qrc)

# 把版本号注入代码 (User-Agent / 应用版本)
target_compile_definitions(gxde-dlna-caster PRIVATE
    APP_VERSION="${PROJECT_VERSION}")

target_link_libraries(gxde-dlna-caster PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::WebSockets
    PkgConfig::DTK2
)

# ---------- 国际化: .ts -> .qm ----------
if(TARGET Qt6::lrelease AND TARGET Qt6::lupdate)
    set(LRELEASE_EXECUTABLE Qt6::lrelease)
    set(LUPDATE_EXECUTABLE Qt6::lupdate)
else()
    find_program(LRELEASE_EXECUTABLE lrelease-qt6 lrelease)
    find_program(LUPDATE_EXECUTABLE lupdate-qt6 lupdate)
endif()

set(TS_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/translations/gxde-dlna-caster.ts
    ${CMAKE_CURRENT_SOURCE_DIR}/translations/gxde-dlna-caster_zh_CN.ts
)

set(QM_FILES "")
if(LRELEASE_EXECUTABLE)
    foreach(ts ${TS_FILES})
        get_filename_component(ts_name ${ts} NAME_WE)
        set(qm ${CMAKE_CURRENT_BINARY_DIR}/translations/${ts_name}.qm)
        add_custom_command(OUTPUT ${qm}
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    ${CMAKE_CURRENT_BINARY_DIR}/translations
            COMMAND ${LRELEASE_EXECUTABLE} ${ts} -qm ${qm}
            DEPENDS ${ts}
        )
        list(APPEND QM_FILES ${qm})
    endforeach()
    add_custom_target(gxde-dlna-caster-translations ALL DEPENDS ${QM_FILES})
else()
    message(WARNING "未找到 lrelease, 跳过 .qm 生成")
endif()

if(LUPDATE_EXECUTABLE)
    add_custom_target(update_translations
        COMMAND ${LUPDATE_EXECUTABLE} -no-obsolete -source-language en
                ${SOURCES} -ts ${TS_FILES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "更新 translations/*.ts (lupdate)"
    )
endif()

# ---------- 安装 ----------
install(TARGETS gxde-dlna-caster RUNTIME DESTINATION bin)
install(FILES ${QM_FILES}
        DESTINATION share/gxde-dlna-caster/translations)
install(FILES application/gxde-dlna-caster.desktop
        DESTINATION share/applications)
install(FILES assets/icons/gxde-dlna-caster.png
        DESTINATION share/icons/hicolor/512x512/apps)
