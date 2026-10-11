import QtQuick

QtObject {
    id: root

    property var host: null
    property var window: null
    property var openDialog: null
    property var saveAsDialog: null

    readonly property var menuGroupOrder: [
        "File", "Edit", "View", "Document", "Production", "Preflight", "Help", "Advanced"
    ]

    // Membership comes from the manifest-backed command descriptors. The
    // order is presentation policy; QML does not maintain a second action set.
    function menuGroups() {
        if (!host) {
            return []
        }

        const present = ({})
        const descriptors = host.commandDescriptors()
        for (let index = 0; index < descriptors.length; ++index) {
            const entry = descriptors[index]
            const disposition = entry.disposition || ""
            if (disposition === "HIDE" || disposition === "STOP-SHIPPING" ||
                    (disposition === "ADVANCED" && !host.allowDeveloperDiagnostics)) {
                continue
            }
            if (entry.menuGroup) {
                present[entry.menuGroup] = true
            }
        }

        const groups = []
        for (let index = 0; index < menuGroupOrder.length; ++index) {
            const group = menuGroupOrder[index]
            if (present[group]) {
                groups.push(group)
                delete present[group]
            }
        }
        for (const group in present) {
            groups.push(group)
        }
        return groups
    }

    function descriptorsForGroup(groupName) {
        if (!host) {
            return []
        }

        const descriptors = host.commandDescriptors()
        const entries = []
        for (let index = 0; index < descriptors.length; ++index) {
            const entry = descriptors[index]
            const disposition = entry.disposition || ""
            if (disposition === "HIDE" || disposition === "STOP-SHIPPING") {
                continue
            }
            if (groupName === "Advanced") {
                if (disposition !== "ADVANCED") {
                    continue
                }
                if (!host.allowDeveloperDiagnostics) {
                    continue
                }
            } else if (entry.menuGroup !== groupName) {
                continue
            } else if (disposition === "ADVANCED" && !host.allowDeveloperDiagnostics) {
                continue
            }
            entries.push(entry)
        }
        return entries
    }

    function labelForEntry(entry) {
        if (!entry || !entry.labelKey) {
            return entry && entry.id ? entry.id : ""
        }
        switch (entry.id) {
        case "actionAbout": return qsTr("About")
        case "actionAutomaticDocumentRefresh": return qsTr("Automatic Document Refresh")
        case "actionBecomeASponsor": return qsTr("Become ASponsor")
        case "actionBleedFixup": return qsTr("Bleed Fixup")
        case "actionBookmarkAutoGenerate": return qsTr("Bookmark Auto Generate")
        case "actionBookmarkExport": return qsTr("Bookmark Export")
        case "actionBookmarkImport": return qsTr("Bookmark Import")
        case "actionBookmarkPage": return qsTr("Bookmark Page")
        case "actionCertificateManager": return qsTr("Certificate Manager")
        case "actionClearRecentFileHistory": return qsTr("Clear Recent File History")
        case "actionClose": return qsTr("Close")
        case "actionCollectDiagnostics": return qsTr("Collect Diagnostics")
        case "actionColorBitonal": return qsTr("Color Bitonal")
        case "actionColorCustom": return qsTr("Color Custom")
        case "actionColorGrayscale": return qsTr("Color Grayscale")
        case "actionColorHighContrast": return qsTr("Color High Contrast")
        case "actionColorInvert": return qsTr("Color Invert")
        case "actionCopyText": return qsTr("Copy Text")
        case "actionCreateBitonalDocument": return qsTr("Create Bitonal Document")
        case "actionCreateEllipse": return qsTr("Create Ellipse")
        case "actionCreateFreehandCurve": return qsTr("Create Freehand Curve")
        case "actionCreateHyperlink": return qsTr("Create Hyperlink")
        case "actionCreateHyperlinkToThisPDFFit": return qsTr("Create Hyperlink To This PDFFit")
        case "actionCreateHyperlinkToThisPDFFitB": return qsTr("Create Hyperlink To This PDFFit B")
        case "actionCreateHyperlinkToThisPDFFitBH": return qsTr("Create Hyperlink To This PDFFit BH")
        case "actionCreateHyperlinkToThisPDFFitBV": return qsTr("Create Hyperlink To This PDFFit BV")
        case "actionCreateHyperlinkToThisPDFFitH": return qsTr("Create Hyperlink To This PDFFit H")
        case "actionCreateHyperlinkToThisPDFFitR": return qsTr("Create Hyperlink To This PDFFit R")
        case "actionCreateHyperlinkToThisPDFFitV": return qsTr("Create Hyperlink To This PDFFit V")
        case "actionCreateHyperlinkToThisPDFXYZ": return qsTr("Create Hyperlink To This PDFXYZ")
        case "actionCreateHyperlinkToThisPDFXYZInheritZoom": return qsTr("Create Hyperlink To This PDFXYZInherit Zoom")
        case "actionCreatePolygon": return qsTr("Create Polygon")
        case "actionCreatePolyline": return qsTr("Create Polyline")
        case "actionCreateRectangle": return qsTr("Create Rectangle")
        case "actionCreateStraightLine": return qsTr("Create Straight Line")
        case "actionCreateTextHighlight": return qsTr("Create Text Highlight")
        case "actionCreateTextSquiggly": return qsTr("Create Text Squiggly")
        case "actionCreateTextStrikeout": return qsTr("Create Text Strikeout")
        case "actionCreateTextUnderline": return qsTr("Create Text Underline")
        case "actionDeselectText": return qsTr("Deselect Text")
        case "actionEncryption": return qsTr("Encryption")
        case "actionExtractImage": return qsTr("Extract Image")
        case "actionFind": return qsTr("Find")
        case "actionFindNext": return qsTr("Find Next")
        case "actionFindPrevious": return qsTr("Find Previous")
        case "actionFirstPageOnRightSide": return qsTr("First Page On Right Side")
        case "actionFitHeight": return qsTr("Fit Height")
        case "actionFitPage": return qsTr("Fit Page")
        case "actionFitWidth": return qsTr("Fit Width")
        case "actionFullscreenMode": return qsTr("Fullscreen Mode")
        case "actionGetSource": return qsTr("Get Source")
        case "actionGoToDocumentEnd": return qsTr("Go To Document End")
        case "actionGoToDocumentStart": return qsTr("Go To Document Start")
        case "actionGotoNextBookmark": return qsTr("Goto Next Bookmark")
        case "actionGoToNextLine": return qsTr("Go To Next Line")
        case "actionGoToNextPage": return qsTr("Go To Next Page")
        case "actionGotoPreviousBookmark": return qsTr("Goto Previous Bookmark")
        case "actionGoToPreviousLine": return qsTr("Go To Previous Line")
        case "actionGoToPreviousPage": return qsTr("Go To Previous Page")
        case "actionInlineText": return qsTr("Inline Text")
        case "actionInsertPageNumbers": return qsTr("Insert Page Numbers")
        case "actionMagnifier": return qsTr("Magnifier")
        case "actionMoveSelection": return qsTr("Move Selection")
        case "actionOpen": return qsTr("Open")
        case "actionOptimize": return qsTr("Optimize")
        case "actionOptimizeImages": return qsTr("Optimize Images")
        case "actionOptions": return qsTr("Options")
        case "actionPageGeometry": return qsTr("Page Geometry")
        case "actionPageLayoutContinuous": return qsTr("Page Layout Continuous")
        case "actionPageLayoutSinglePage": return qsTr("Page Layout Single Page")
        case "actionPageLayoutTwoColumns": return qsTr("Page Layout Two Columns")
        case "actionPageLayoutTwoPages": return qsTr("Page Layout Two Pages")
        case "actionPrint": return qsTr("Print")
        case "actionProperties": return qsTr("Properties")
        case "actionQuit": return qsTr("Quit")
        case "actionRedo": return qsTr("Redo")
        case "actionRemoveExternalLinks": return qsTr("Remove External Links")
        case "actionRender_to_Images": return qsTr("Render to Images")
        case "actionRendering_Errors": return qsTr("Rendering Errors")
        case "actionRenderOptionAntialiasing": return qsTr("Render Option Antialiasing")
        case "actionRenderOptionDisplayAnnotations": return qsTr("Render Option Display Annotations")
        case "actionRenderOptionDisplayRenderTimes": return qsTr("Render Option Display Render Times")
        case "actionRenderOptionIgnoreOptionalContentSettings": return qsTr("Render Option Ignore Optional Content Settings")
        case "actionRenderOptionSmoothPictures": return qsTr("Render Option Smooth Pictures")
        case "actionRenderOptionTextAntialiasing": return qsTr("Render Option Text Antialiasing")
        case "actionResetToFactorySettings": return qsTr("Reset To Factory Settings")
        case "actionRotateLeft": return qsTr("Rotate Left")
        case "actionRotateRight": return qsTr("Rotate Right")
        case "actionSanitize": return qsTr("Sanitize")
        case "actionSave": return qsTr("Save")
        case "actionSave_As": return qsTr("Save As")
        case "actionScreenshot": return qsTr("Screenshot")
        case "actionSelectTable": return qsTr("Select Table")
        case "actionSelectText": return qsTr("Select Text")
        case "actionSelectTextAll": return qsTr("Select Text All")
        case "actionSend_by_E_Mail": return qsTr("Send by E Mail")
        case "actionShow_Text_Blocks": return qsTr("Show Text Blocks")
        case "actionShow_Text_Lines": return qsTr("Show Text Lines")
        case "actionStickyNoteComment": return qsTr("Sticky Note Comment")
        case "actionStickyNoteHelp": return qsTr("Sticky Note Help")
        case "actionStickyNoteInsert": return qsTr("Sticky Note Insert")
        case "actionStickyNoteKey": return qsTr("Sticky Note Key")
        case "actionStickyNoteNewParagraph": return qsTr("Sticky Note New Paragraph")
        case "actionStickyNoteNote": return qsTr("Sticky Note Note")
        case "actionStickyNoteParagraph": return qsTr("Sticky Note Paragraph")
        case "actionUndo": return qsTr("Undo")
        case "actionZoom_In": return qsTr("Zoom In")
        case "actionZoom_Out": return qsTr("Zoom Out")
        default: return entry.id
        }
    }

    function commandEnabled(commandId) {
        return host ? host.isCommandEnabled(commandId) : false
    }

    function invoke(commandId) {
        if (!host) {
            return
        }
        if (commandId === "actionOpen" && openDialog) {
            if (host.focusRestoration) {
                host.focusRestoration.remember(window.activeFocusItem)
            }
            openDialog.open()
            return
        }
        if (commandId === "actionSave_As" && saveAsDialog) {
            if (host.focusRestoration) {
                host.focusRestoration.remember(window.activeFocusItem)
            }
            saveAsDialog.open()
            return
        }
        host.invokeCommand(commandId)
    }
}
