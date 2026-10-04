import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Pane {
    id: root

    objectName: "comparePane"

    property var host: editorHost

    // Read-only projection of Core's own comparison facts. The pane renders what
    // EditorHost.compareReview() resolves from the run result; it derives no
    // identity and calls no mutator, writer or PDFRepairTransaction::apply().
    readonly property var review: host ? host.compareReview : null
    readonly property bool blocked: review !== null && review.blocked
    readonly property bool available: review !== null && review.available

    padding: 8

    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Compare")
    Accessible.description: qsTr("Compare the correction's before and after artifacts, its finding delta, the attributes it preserved and the risk it left unresolved.")

    function digest(value) {
        return value === undefined || value === null ? "" : String(value).substring(0, 12)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        // Guard first: a stale preview or a mismatched plan digest is never presented as
        // current and is never silently refreshed.
        GroupBox {
            objectName: "compareStaleGuard"
            Layout.fillWidth: true
            visible: root.blocked
            title: qsTr("Comparison not current")
            Accessible.name: qsTr("Comparison not current")

            Label {
                objectName: "compareBlockedReason"
                anchors.fill: parent
                wrapMode: Text.WordWrap
                text: root.review === null ? "" : root.review.blockedReason
                Accessible.name: qsTr("Why the comparison is blocked")
            }
        }

        Label {
            objectName: "compareAvailability"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: !root.available
            text: qsTr("No correction is planned for this document yet. Plan one in the Fix workspace to compare it.")
            Accessible.name: qsTr("No comparison available")
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: root.available

            GroupBox {
                objectName: "compareArtifactsGroup"
                Layout.fillWidth: true
                title: qsTr("Before / after artifacts")
                Accessible.name: qsTr("Before and after artifacts")

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 3

                    Label {
                        objectName: "compareArtifacts"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: root.review === null ? "" : qsTr("Source %1 → candidate %2 → published %3")
                            .arg(root.digest(root.review.before.sourceSha256))
                            .arg(root.digest(root.review.after.candidateSha256))
                            .arg(root.digest(root.review.after.publishedSha256))
                        Accessible.name: qsTr("Artifact digests")
                    }

                    Label {
                        objectName: "comparePlanIdentity"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: root.review === null ? "" : qsTr("Plan %1 · revision %2 · recipe %3 · review %4 · plan matches this revision: %5")
                            .arg(root.digest(root.review.plan.planDigest))
                            .arg(root.digest(root.review.before.plannedRevision))
                            .arg(root.review.plan.recipeId)
                            .arg(root.review.after.reviewDecision)
                            .arg(root.review.plan.planIsCurrent ? qsTr("yes") : qsTr("no"))
                        Accessible.name: qsTr("Comparison plan identity")
                    }
                }
            }

            GroupBox {
                objectName: "compareDeltaGroup"
                Layout.fillWidth: true
                title: qsTr("Technical finding delta")
                Accessible.name: qsTr("Technical finding delta")

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 3

                    Label {
                        objectName: "compareDeltaSummary"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: root.review === null ? "" : qsTr("Comparison complete: %1 · cleared %2 · remaining %3 · introduced %4 · not fully rechecked %5")
                            .arg(root.review.findingDelta.compared ? qsTr("yes") : qsTr("no"))
                            .arg(root.review.findingDelta.resolved.length)
                            .arg(root.review.findingDelta.unchanged.length)
                            .arg(root.review.findingDelta.introduced.length)
                            .arg(root.review.findingDelta.incomplete.length)
                        Accessible.name: qsTr("Finding delta summary")
                    }

                    Repeater {
                        objectName: "compareMaterialDeltas"
                        model: root.review === null ? [] : root.review.materialDeltas

                        delegate: RowLayout {
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true

                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                text: qsTr("%1 · %2").arg(modelData.kind).arg(root.digest(modelData.findingId))
                                Accessible.name: qsTr("Material delta %1").arg(modelData.kind)
                            }

                            Button {
                                text: qsTr("Navigate")
                                enabled: root.host !== null && !root.blocked
                                onClicked: if (root.host) root.host.navigateCompareDelta(index)
                                Accessible.name: qsTr("Navigate to material delta %1").arg(modelData.findingId)
                            }
                        }
                    }
                }
            }

            GroupBox {
                objectName: "comparePreservedGroup"
                Layout.fillWidth: true
                title: qsTr("Preserved attributes")
                Accessible.name: qsTr("Preserved attributes")

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 3

                    Label {
                        objectName: "comparePreservedSummary"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: root.review === null ? "" : qsTr("Declared change surface: %1 · findings preserved unchanged: %2 · carried forward: %3 · source retention %4")
                            .arg(root.review.preserved.declaredChangeAttributes.length > 0
                                ? root.review.preserved.declaredChangeAttributes.join(", ")
                                : qsTr("none declared"))
                            .arg(root.review.preserved.unchangedFindings.length)
                            .arg(root.review.preserved.carriedForwardFindings.length)
                            .arg(root.review.preserved.savePolicyMode.length > 0
                                ? root.review.preserved.savePolicyMode
                                : qsTr("unknown"))
                        Accessible.name: qsTr("Preserved attributes summary")
                    }
                }
            }

            GroupBox {
                objectName: "compareRiskGroup"
                Layout.fillWidth: true
                title: qsTr("Unresolved risk")
                Accessible.name: qsTr("Unresolved risk")

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 3

                    Label {
                        objectName: "compareRiskSummary"
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: root.review === null ? "" : qsTr("Declared risk: %1 · introduced findings: %2 · not fully rechecked: %3 · reported messages: %4")
                            .arg(root.review.unresolvedRisk.level.length > 0
                                ? root.review.unresolvedRisk.level
                                : qsTr("none declared"))
                            .arg(root.review.unresolvedRisk.introducedFindings.length)
                            .arg(root.review.unresolvedRisk.incompleteFindings.length)
                            .arg(root.review.unresolvedRisk.messages.length)
                        Accessible.name: qsTr("Unresolved risk summary")
                    }
                }
            }
        }

        Item { Layout.fillHeight: true }
    }
}
