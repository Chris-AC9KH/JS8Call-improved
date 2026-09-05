cmake_minimum_required(VERSION 3.22 FATAL_ERROR)
#------------------------------------------------------------------------------#
#
#   JS8Call Build:
#
#     1) mkdir build
#     2) cd build
#     3) cmake ..
#     4) make
#
#     For subsequent builds, just skip directly to step 4.
#
#------------------------------------------------------------------------------#

#------------------------------------------------------------------------------#
# Policies; add to this list as required:
#
#  - CMP0144: find_package uses upper-case <PACKAGENAME>_ROOT variables.
#  - CMP0167: Require a Boost 1.70+ installation, with cmake support.
#  - CMP0177: DESTINATION paths are normalized.
#------------------------------------------------------------------------------#

foreach(policy CMP0144 CMP0167 CMP0177)
  if(POLICY ${policy})
    cmake_policy(SET ${policy} NEW)
  endif()
endforeach()

#------------------------------------------------------------------------------#
# Configuration defaults that we allow to be overridden on the cmake
# command line.
#------------------------------------------------------------------------------#

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if (NOT CMAKE_PREFIX_PATH)
  set(CMAKE_PREFIX_PATH ${CMAKE_SOURCE_DIR}/../js8libs)
endif()

#------------------------------------------------------------------------------#
# Configuration defaults that we don't allow to be overridden.
#------------------------------------------------------------------------------#

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET 14.4)

#------------------------------------------------------------------------------#
# Project configuration. The software build version is set here. If the version
# is set to 0.0.0 the build will be unversioned and marked as "Development"
#------------------------------------------------------------------------------#

project(
  JS8Call
  VERSION   0.0.0
  LANGUAGES C CXX
)

set(PROJECT_VENDOR "Jordan Sherer, KN4CRD")
set(PROJECT_CONTACT "Jordan Sherer <kn4crd@gmail.com>")
set(PROJECT_COPYRIGHT "Copyright (C) 2001-2018 by Joe Taylor, K1JT, (C) 2018 by Jordan Sherer, KN4CRD, (C) of contributed work is owned by individual contributors")
set(PROJECT_HOMEPAGE https://groups.io/g/js8call)
set(PROJECT_SUMMARY_DESCRIPTION "${PROJECT_NAME} - Digital Modes for Weak Signal Communications in Amateur Radio.")
set(PROJECT_DESCRIPTION "${PROJECT_SUMMARY_DESCRIPTION}
 ${PROJECT_NAME} is a computer program designed to facilitate amateur radio communication using very weak signals.")

#------------------------------------------------------------------------------#
# Add our own CMake modules.
#------------------------------------------------------------------------------#

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/CMake/Modules")

#------------------------------------------------------------------------------#
# CMake Script Includes
#------------------------------------------------------------------------------#

include(CheckTypeSize)
include(CheckSymbolExists)
include(CMakeDependentOption)
include(GenerateExportHeader)
include(GNUInstallDirs)

#------------------------------------------------------------------------------#
# Options and features. Some of these directly affect compilation by being
# define in js8call_config.h.in, which makes them available to the C/C++
# preprocessor.
#------------------------------------------------------------------------------#

option(WSJT_QDEBUG_TO_FILE     "Redirect Qt debugging messages to a trace file.")
option(WSJT_HAMLIB_TRACE       "Debugging option that turns on minimal Hamlib internal diagnostics.")
option(WSJT_RIG_NONE_CAN_SPLIT "Allow split operation with \"None\" as rig.")

cmake_dependent_option(
  WSJT_HAMLIB_VERBOSE_TRACE
  "Debugging option that turns on full Hamlib internal diagnostics."
  OFF
  WSJT_HAMLIB_TRACE
  OFF)

cmake_dependent_option(
  WSJT_QDEBUG_IN_RELEASE
  "Leave Qt debugging statements in Release configuration."
  OFF
  "NOT (CMAKE_BUILD_TYPE STREQUAL Debug)"
  OFF)

cmake_dependent_option(
  WSJT_ENABLE_EXPERIMENTAL_FEATURES
  "Enable features not fully ready for public releases."
  ON
  "CMAKE_BUILD_TYPE STREQUAL Debug"
  OFF)

#------------------------------------------------------------------------------#
# Ensure we have required library dependencies; we'll need headers from the
# Boost and FFTW3 libraries.
#------------------------------------------------------------------------------#

find_package(Boost 1.77 REQUIRED)
find_package(FFTW3      REQUIRED COMPONENTS single threads)
find_package(Hamlib     REQUIRED)
find_package(Qt6   6.11 REQUIRED COMPONENTS Multimedia Network SerialPort Widgets WebSockets)

include_directories(${Boost_INCLUDE_DIRS})
include_directories(${FFTW3_INCLUDE_DIRS})

#------------------------------------------------------------------------------#
# OSX requires an icon file resource.
#------------------------------------------------------------------------------#

if (APPLE)
  set(ICON_FILE ${CMAKE_PROJECT_NAME}.icns)
  set(ICON_SRCS
    icons/Darwin/${CMAKE_PROJECT_NAME}.iconset/icon_16x16.png
    icons/Darwin/${CMAKE_PROJECT_NAME}.iconset/icon_16x16@2x.png
    icons/Darwin/${CMAKE_PROJECT_NAME}.iconset/icon_32x32.png
    icons/Darwin/${CMAKE_PROJECT_NAME}.iconset/icon_32x32@2x.png
    icons/Darwin/${CMAKE_PROJECT_NAME}.iconset/icon_128x128.png
    icons/Darwin/${CMAKE_PROJECT_NAME}.iconset/icon_128x128@2x.png
    icons/Darwin/${CMAKE_PROJECT_NAME}.iconset/icon_256x256.png
    icons/Darwin/${CMAKE_PROJECT_NAME}.iconset/icon_256x256@2x.png
    icons/Darwin/${CMAKE_PROJECT_NAME}.iconset/icon_512x512.png
    icons/Darwin/${CMAKE_PROJECT_NAME}.iconset/icon_512x512@2x.png
  )
  add_custom_command(
    OUTPUT ${ICON_FILE}
    COMMAND iconutil -c icns --output "${CMAKE_BINARY_DIR}/${ICON_FILE}" "${CMAKE_SOURCE_DIR}/icons/Darwin/${CMAKE_PROJECT_NAME}.iconset"
    DEPENDS ${ICON_SRCS}
    COMMENT "Building Icons"
  )
  set_source_files_properties(${ICON_FILE} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
endif (APPLE)

#------------------------------------------------------------------------------#
# Create a target that's the same as our project name; that'll be our
# created executable.
#------------------------------------------------------------------------------#

set(TARGET ${PROJECT_NAME})

qt_add_executable(${TARGET} MACOSX_BUNDLE WIN32 ${ICON_FILE})

#------------------------------------------------------------------------------#
# OSX requires a number of settings in the plist file.
#------------------------------------------------------------------------------#

set_target_properties(
  ${TARGET} PROPERTIES
  MACOSX_BUNDLE_INFO_PLIST             "${CMAKE_CURRENT_SOURCE_DIR}/resources/MacOS/Info.plist.in"
  MACOSX_BUNDLE_ICON_FILE              "${ICON_FILE}"
  MACOSX_BUNDLE_BUNDLE_VERSION         "${PROJECT_VERSION}"
  MACOSX_BUNDLE_SHORT_VERSION_STRING   "${PROJECT_VERSION}"
  MACOSX_BUNDLE_COPYRIGHT              "${PROJECT_COPYRIGHT}"
)

#------------------------------------------------------------------------------#
# Set CMAKE_BUILD to 1 globally for use by things that need to know
# they're being compiled under cmake, then preprocess and precompile
# the configuration header.
#------------------------------------------------------------------------------#
find_program(GIT_EXECUTABLE git)
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
else()
    set(GIT_HASH "nogit")
endif()

if(WIN32)
    # Prefer environment variables on Windows
    set(HOSTNAME "$ENV{COMPUTERNAME}")
    set(USERNAME "$ENV{USERNAME}")
else()
    execute_process(
        COMMAND hostname
        OUTPUT_VARIABLE HOSTNAME
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    execute_process(
        COMMAND whoami
        OUTPUT_VARIABLE USERNAME
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()
    
target_compile_definitions(${TARGET}
    PRIVATE
    CMAKE_BUILD
    JS8CALL_VERSION=\"${PROJECT_VERSION}\"
    GITSHORT=\"${GIT_HASH}\"
    MACHINE=\"${HOSTNAME}\"
    UNAME=\"${USERNAME}\"
)

string(TIMESTAMP TODAY_DATE "%m-%d-%Y")

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/JS8_Include/js8call_config.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/js8call_config.h"
)

target_precompile_headers(
  ${TARGET} PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}/js8call_config.h
)

#------------------------------------------------------------------------------#
# Suppress noisy narrowing-conversion warnings from vendored sqlite3.c.
# It's third-party code we don't maintain; no need to see 78 warnings
# on every build.
#------------------------------------------------------------------------------#
set_source_files_properties(
  vendor/sqlite3/sqlite3.c
  PROPERTIES
    COMPILE_OPTIONS "-Wno-shorten-64-to-32"
)

#------------------------------------------------------------------------------#
# Source files common to all platforms.
#------------------------------------------------------------------------------#

target_sources(
  ${TARGET} PRIVATE
  vendor/sqlite3/sqlite3.c
  JS8_Audio/AudioDevice.cpp
  JS8_Audio/BWFFile.cpp
  JS8_Audio/NotificationAudio.cpp
  JS8_Audio/NotificationSoundOutput.cpp
  JS8_Audio/SoundInput.cpp
  JS8_Audio/SoundOutput.cpp
  JS8_JSC/JSC.cpp
  JS8_JSC/JSC_checker.cpp
  JS8_JSC/JSC_list.cpp
  JS8_JSC/JSC_map.cpp
  JS8_Logbook/ADIF.cpp
  JS8_Logbook/CountryDat.cpp
  JS8_Logbook/CountriesWorked.cpp
  JS8_Logbook/LogBook.cpp
  JS8_Main/ActivityDB.cpp
  JS8_Main/AprsInboundRelay.cpp
  JS8_Main/APRSISClient.cpp
  JS8_Main/AttenuationSlider.cpp
  JS8_Main/Bands.cpp
  JS8_Main/CallsignValidator.cpp
  JS8_Main/CandidateKeyFilter.cpp
  JS8_Main/CPlotter.cpp
  JS8_Main/DirectedMessageHighlighter.cpp
  JS8_Main/DirectedMessageParser.cpp
  JS8_Main/DriftingDateTime.cpp
  JS8_Main/Flatten.cpp
  JS8_Main/ForeignKeyDelegate.cpp
  JS8_Main/FrequencyLineEdit.cpp
  JS8_Main/FrequencyList.cpp
  JS8_Main/Geodesic.cpp
  JS8_Main/HBBlockingDB.cpp
  JS8_Main/HelpTextWindow.cpp
  JS8_Main/IARURegions.cpp
  JS8_Main/Inbox.cpp
  JS8_Main/JS8MessageBox.cpp
  JS8_Main/Message.cpp
  JS8_Main/MessageClient.cpp
  JS8_Main/MessageError.cpp
  JS8_Main/MessageServer.cpp
  JS8_Main/MetaDataRegistry.cpp
  JS8_Main/MultiSettings.cpp
  JS8_Main/RDP.cpp
  JS8_Main/StationList.cpp
  JS8_Main/TxLoop.cpp
  JS8_Main/fileutils.cpp
  JS8_Main/main.cpp
  JS8_Main/Modes.cpp
  JS8_Main/MsgNotifyDB.cpp
  JS8_Main/PillRenderer.cpp
  JS8_Main/ProcessThread.cpp
  JS8_Main/qt_helpers.cpp
  JS8_Main/Radio.cpp
  JS8_Main/RadioMetaType.cpp
  JS8_Main/revision_utils.cpp
  JS8_Main/SelfDestructMessageBox.cpp
  JS8_Main/SignalMeter.cpp
  JS8_Main/TraceFile.cpp
  JS8_Main/TransmitTextSourceMirror.cpp
  JS8_Main/TransmitTextEdit.cpp
  JS8_Main/TwoPhaseSignal.cpp
  JS8_Main/Varicode.cpp
  JS8_Mainwindow/buildQueryMenu.cpp
  JS8_Mainwindow/checkVersion.cpp
  JS8_Mainwindow/dataSink.cpp
  JS8_Mainwindow/displayBandActivity.cpp
  JS8_Mainwindow/displayCallActivity.cpp
  JS8_Mainwindow/ensureMessageDock.cpp
  JS8_Mainwindow/initializeDummyData.cpp
  JS8_Mainwindow/initializeGroupMessage.cpp
  JS8_Mainwindow/networkMessage.cpp
  JS8_Mainwindow/processBufferedActivity.cpp
  JS8_Mainwindow/processCommandActivity.cpp
  JS8_Mainwindow/processDecodeEvent.cpp
  JS8_Mainwindow/processHeartbeatRateLimit.cpp
  JS8_Mainwindow/pushNotificationHandler.cpp
  JS8_Mainwindow/processRxActivity.cpp
  JS8_Mainwindow/UI_Constructor.cpp
  JS8_Mainwindow/confirmThenEnqueueMessage.cpp
  JS8_Mode/DecodedText.cpp
  JS8_Mode/Detector.cpp
  JS8_Mode/FrequencyTracker.cpp
  JS8_Mode/JS8.cpp
  JS8_Mode/JS8Submode.cpp
  JS8_Mode/Modulator.cpp
  JS8_Network/NetworkServerLookup.cpp
  JS8_Network/PSKReporter.cpp
  JS8_Network/SpotClient.cpp
  JS8_Network/TCPClient.cpp
  JS8_Transceiver/DXLabSuiteCommanderTransceiver.cpp
  JS8_Transceiver/EmulateSplitTransceiver.cpp
  JS8_Transceiver/HamlibTransceiver.cpp
  JS8_Transceiver/HRDTransceiver.cpp
  JS8_Transceiver/PollingTransceiver.cpp
  JS8_Transceiver/Transceiver.cpp
  JS8_Transceiver/TransceiverBase.cpp
  JS8_Transceiver/TransceiverFactory.cpp
  JS8_UDP/NetworkMessage.cpp
  JS8_UDP/WSJTXMessageClient.cpp
  JS8_UDP/WSJTXMessageMapper.cpp
  JS8_UI/Configuration.cpp
  JS8_UI/About.cpp
  JS8_UI/LogQSO.cpp
  JS8_UI/mainwindow.cpp
  JS8_UI/MessageReplyDialog.cpp
  JS8_UI/MessagePanel.cpp
  JS8_UI/WF.cpp
  JS8_UI/WideGraph.cpp
  JS8_Widgets/CheckableItemComboBox.cpp
  JS8_Widgets/LazyFillComboBox.cpp
)

#------------------------------------------------------------------------------#
# Link libraries common to all platforms.
#------------------------------------------------------------------------------#

target_link_libraries(
  ${TARGET} PRIVATE
  ${FFTW3_LIBRARIES}
  Hamlib::Hamlib
  Qt::Multimedia
  Qt::Network
  Qt::SerialPort
  Qt::Widgets
  Qt::WebSockets
)

#------------------------------------------------------------------------------#
# Resources for country data and eclipse dates, used by the log book and the
# PSK reporter, respectively.
#------------------------------------------------------------------------------#

qt_add_resources(
  ${TARGET} "data"
  FILES
  JS8_Include/cty.dat
  JS8_Include/eclipse.txt
)

#------------------------------------------------------------------------------#
# Waterfall palette resources.
#------------------------------------------------------------------------------#

qt_add_resources(
  ${TARGET} "palettes"
  BASE      "Palettes"
  PREFIX   "/Palettes"
  FILES
  Palettes/Banana.pal
  Palettes/Blue1.pal
  Palettes/Blue2.pal
  Palettes/Blue3.pal
  Palettes/Brown.pal
  Palettes/Cyan1.pal
  Palettes/Cyan2.pal
  Palettes/Cyan3.pal
  Palettes/Default.pal
  Palettes/Digipan.pal
  Palettes/Fldigi.pal
  Palettes/Gray1.pal
  Palettes/Gray2.pal
  Palettes/Green1.pal
  Palettes/Green2.pal
  Palettes/Jungle.pal
  Palettes/Linrad.pal
  Palettes/Negative.pal
  Palettes/Orange.pal
  Palettes/Pink.pal
  Palettes/Rainbow.pal
  Palettes/Scope.pal
  Palettes/Sunburst.pal
  Palettes/VK4BDJ.pal
  Palettes/YL2KF.pal
  Palettes/Yellow1.pal
  Palettes/Yellow2.pal
  Palettes/ZL1FZ.pal
)

#------------------------------------------------------------------------------#
# Compiler setup. OSX will by default choose the correct compiler flags;
# on other platforms we'll probably need to expand on this by platform.
#------------------------------------------------------------------------------#

set (CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -Wall -Wextra")
set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -fexceptions -frtti")

#------------------------------------------------------------------------------#
# In order to execute the macdeployqt and windeployqt commands, we'll need
# to know where they live, which is the same location as the qmake binary.
#------------------------------------------------------------------------------#

get_target_property(QMAKE_EXECUTABLE Qt::qmake IMPORTED_LOCATION)
get_filename_component(QT_BIN_DIR "${QMAKE_EXECUTABLE}" DIRECTORY)

#------------------------------------------------------------------------------#
# On OSX, all we have to do at this point is deploy in overwrite mode to
# have all the needed library dependencies injected into the target.
#------------------------------------------------------------------------------#

if (APPLE)
  find_program(MACDEPLOYQT macdeployqt HINTS "${QT_BIN_DIR}")
  add_custom_command(
    TARGET ${TARGET}
    POST_BUILD
    COMMAND "${MACDEPLOYQT}"
      "$<TARGET_FILE_DIR:${TARGET}>/../.."
      -always-overwrite
    COMMENT "Running macdeployqt..."
  )
endif (APPLE)

if (WIN32)

  # Windows icons require an RC file.

  target_sources(${TARGET} PRIVATE JS8_Include/js8call.rc)

  # Find the windeployqt command and run it against the generated binary,
  # which should handle all the gnarly bits.

  find_program(WINDEPLOYQT NAMES windeployqt HINTS "${QT_BIN_DIR}")
  add_custom_command(
    TARGET ${TARGET}
    POST_BUILD
    COMMAND "${WINDEPLOYQT}" "$<TARGET_FILE_DIR:${TARGET}>"
    --libdir JS8Call
    --plugindir JS8Call
    --no-translations
    COMMENT "Running windeployqt..."
  )
endif (WIN32)

#------------------------------------------------------------------------------#
