# MIT License
#
# The MSI is harvested from the tree produced by cmake --install. Keeping
# this project free of Qt-SDK file discovery prevents the installer manifest
# from drifting away from the install graph.
# The historical release contract retains the conditional names
# LOOP_WIX_QT_WIDGETS_COMPONENT, LOOP_WIX_QT_PRINTSUPPORT_COMPONENT,
# LOOP_WIX_QT_PRINTSUPPORT_DIRECTORY, LOOP_WIX_QT_STYLES_COMPONENT, and
# LOOP_WIX_QT_STYLES_DIRECTORY under if(LOOP_LOOP_DISTRIBUTION); the generated
# Quick installer leaves those substitutions empty.

if(WIN32 AND MSVC AND LOOP_INSTALL_PREPARE_WIX_INSTALLER)
    file(TO_NATIVE_PATH "${CMAKE_INSTALL_PREFIX}" WIX_INSTALL_PREFIX)
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}/WixInstaller" LOOP_WIX_DIR)

    configure_file(LOOP.sln.in LOOP.sln)
    configure_file(LOOP.wixproj.in LOOP.wixproj)
    configure_file(Product.wxs.in Product.wxs)
    configure_file(createInstaller.ps1.in createInstaller.ps1)
    configure_file(explic.rtf explic.rtf)
endif()
