import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Pane {
    id: root

    objectName: "comparePane"

    property var host: editorHost

    readonly property var review: host ? host.compareReview : null
    readonly property bool blocked: review !== null && review.blocked
    readonly property bool available: review !== null && review.available

    padding: 8

    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Compare")
    Accessible.description: qsTr("Compare the correction's before and after artifacts, its finding delta, the attributes it preserved and the risk it left unresolved.")

    function identity(value) {
        return value === undefined || value === null || String(value).length === 0
            ? qsTr("not available") : String(value)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        GroupBox {
            objectName: "compareStaleGuard"
            Layout.fillWidth: true
            visible: root.blocked
            title: qsTr("Comparison not current")
            Accessible.name: qsTr("Comparison not current")

            Label {
                objectName: "compareBlockedReason"
                anchors.fill: parent
                wrapMode: Text.Wrap
                text: root.review === null ? "" : root.review.blockedReason
                Accessible.name: qsTr("Why the comparison is blocked")
                Accessible.description: text
            }
        }

        Label {
            objectName: "compareAvailability"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            visible: !root.available
            text: qsTr("No correction is planned for this document yet. Plan one in the Fix workspace to compare it.")
            Accessible.name: qsTr("No comparison available")
        }

        ScrollView {
            id: compareScroll
            objectName: "compareDetailsScroll"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.available
            clip: true
            contentWidth: availableWidth

            ColumnLayout {
                width: compareScroll.availableWidth
                spacing: 8

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
                            wrapMode: Text.Wrap
                            text: root.review === null ? "" : qsTr("Source SHA-256: %1\nCandidate SHA-256: %2\nPublished SHA-256: %3")
                                .arg(root.identity(root.review.before.sourceSha256))
                                .arg(root.identity(root.review.after.candidateSha256))
                                .arg(root.identity(root.review.after.publishedSha256))
                            Accessible.name: qsTr("Artifact digests")
                            Accessible.description: text
                        }

                        Label {
                            objectName: "comparePlanIdentity"
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: root.review === null ? "" : qsTr("Plan digest: %1\nPlanned revision: %2\nRecipe: %3 - review: %4 - plan matches this revision: %5")
                                .arg(root.identity(root.review.plan.planDigest))
                                .arg(root.identity(root.review.before.plannedRevision))
                                .arg(root.review.plan.recipeId)
                                .arg(root.review.after.reviewDecision)
                                .arg(root.review.plan.planIsCurrent ? qsTr("yes") : qsTr("no"))
                            Accessible.name: qsTr("Comparison plan identity")
                            Accessible.description: text
                        }

                        Label {
                            objectName: "compareReviewIdentity"
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: root.review === null ? "" : qsTr("Reviewed plan digest: %1\nPublished plan digest: %2")
                                .arg(root.identity(root.review.plan.reviewedPlanDigest))
                                .arg(root.identity(root.review.plan.publishedPlanDigest))
                            Accessible.name: qsTr("Reviewed and published plan identity")
                            Accessible.description: text
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
                            wrapMode: Text.Wrap
                            text: root.review === null ? "" : qsTr("Comparison complete: %1 · cleared %2 · remaining %3 · introduced %4 · not fully rechecked %5")
                                .arg(root.review.findingDelta.compared ? qsTr("yes") : qsTr("no"))
                                .arg(root.review.findingDelta.resolved.length)
                                .arg(root.review.findingDelta.unchanged.length)
                                .arg(root.review.findingDelta.introduced.length)
                                .arg(root.review.findingDelta.incomplete.length)
                            Accessible.name: qsTr("Finding delta summary")
                            Accessible.description: text
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
                                    wrapMode: Text.Wrap
                                    objectName: "compareDeltaIdentity_" + index
                                    text: qsTr("%1 · %2").arg(modelData.kind).arg(root.identity(modelData.findingId))
                                    Accessible.name: qsTr("Material delta %1").arg(modelData.kind)
                                    Accessible.description: text
                                }

                                Button {
                                    objectName: "compareNavigateDelta_" + index
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
                            wrapMode: Text.Wrap
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
                            Accessible.description: text
                        }

                        Label {
                            objectName: "comparePreservedDetails"
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: root.review === null ? "" : qsTr("Unchanged finding IDs: %1\nCarried-forward finding IDs: %2\nSource retention rationale: %3")
                                .arg(root.review.preserved.unchangedFindings.length > 0
                                    ? root.review.preserved.unchangedFindings.join(", ") : qsTr("none reported"))
                                .arg(root.review.preserved.carriedForwardFindings.length > 0
                                    ? root.review.preserved.carriedForwardFindings.join(", ") : qsTr("none reported"))
                                .arg(root.identity(root.review.preserved.savePolicyRationale))
                            Accessible.name: qsTr("Preserved findings and source retention rationale")
                            Accessible.description: text
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
                            wrapMode: Text.Wrap
                            text: root.review === null ? "" : qsTr("Declared risk: %1 · introduced findings: %2 · not fully rechecked: %3 · reported messages: %4")
                                .arg(root.review.unresolvedRisk.level.length > 0
                                    ? root.review.unresolvedRisk.level
                                    : qsTr("none declared"))
                                .arg(root.review.unresolvedRisk.introducedFindings.length)
                                .arg(root.review.unresolvedRisk.incompleteFindings.length)
                                .arg(root.review.unresolvedRisk.messages.length)
                            Accessible.name: qsTr("Unresolved risk summary")
                            Accessible.description: text
                        }

                        Label {
                            objectName: "compareRiskDetails"
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: root.review === null ? "" : qsTr("Introduced finding IDs: %1\nRecheck incomplete finding IDs: %2\nStep incomplete finding IDs: %3\nReported messages: %4")
                                .arg(root.review.unresolvedRisk.introducedFindings.length > 0
                                    ? root.review.unresolvedRisk.introducedFindings.join(", ") : qsTr("none reported"))
                                .arg(root.review.findingDelta.incomplete.length > 0
                                    ? root.review.findingDelta.incomplete.join(", ") : qsTr("none reported"))
                                .arg(root.review.unresolvedRisk.incompleteFindings.length > 0
                                    ? root.review.unresolvedRisk.incompleteFindings.join(", ") : qsTr("none reported"))
                                .arg(root.review.unresolvedRisk.messages.length > 0
                                    ? root.review.unresolvedRisk.messages.join("\n") : qsTr("none reported"))
                            Accessible.name: qsTr("Unresolved findings and messages")
                            Accessible.description: text
                        }
                    }
                }
            }
        }
    }
}
