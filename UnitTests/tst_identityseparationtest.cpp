// MIT License
//
// Copyright (c) 2018-2025 Jakub Melka and Contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// Guard against the identity-type collision that broke the Pdf4QtLibCore build:
// two different structs named pdf::PDFArtifactIdentity, both in the same target,
// one describing a persisted artifact and one an in-session document.
//
// Including both headers in a single translation unit is the point of this test.
// If the two concepts are ever given the same name again, this file stops
// compiling long before the rest of the library does.

#include "pdfartifactidentity.h"
#include "pdfdocumentcontext.h"
#include "pdfoperationhistory.h"
#include "pdfoperationhistorystore.h"

#include <QtTest>

#include <type_traits>

static_assert(!std::is_same_v<pdf::PDFArtifactIdentity, pdf::PDFDocumentIdentity>,
              "Persisted artifact identity and in-session document identity must stay distinct types");

static_assert(std::is_same_v<pdf::PDFRevisionToken, pdf::PDFRevisionIdentity>,
              "PDFRevisionToken must remain an alias of PDFRevisionIdentity, not a second type");

static_assert(!std::is_same_v<pdf::PDFRevisionToken, pdf::PDFArtifactIdentity>,
              "Revision tokens must not collapse to persisted PDFArtifactIdentity");

static_assert(!std::is_same_v<pdf::PDFRevisionToken, pdf::PDFDocumentIdentity>,
              "Revision tokens include document identity plus revision/cache/profile fences");

static_assert(std::is_same_v<decltype(std::declval<pdf::PDFOperationHistoryExecution>().input),
                             pdf::PDFArtifactIdentity>,
              "Operation-history executions must reference persisted PDFArtifactIdentity inputs");

static_assert(std::is_same_v<decltype(std::declval<pdf::PDFOperationHistoryEvent>().output),
                             std::optional<pdf::PDFArtifactIdentity>>,
              "Operation-history events must reference persisted PDFArtifactIdentity outputs");

static_assert(!std::is_same_v<decltype(std::declval<pdf::PDFOperationHistoryExecution>().input),
                              pdf::PDFDocumentIdentity>,
              "Operation-history inputs must not collapse to PDFDocumentIdentity");

static_assert(!std::is_same_v<std::decay_t<decltype(std::declval<pdf::PDFOperationHistoryEvent>().output.value())>,
                              pdf::PDFDocumentIdentity>,
              "Operation-history outputs must not collapse to PDFDocumentIdentity");

class IdentitySeparationTest : public QObject
{
    Q_OBJECT

private slots:
    void persistedArtifactIdentity_hasStorageFields();
    void documentIdentity_hasInSessionFields();
    void documentIdentity_fromNullDocumentIsInvalid();
    void revisionIdentity_carriesDocumentIdentity();
    void revisionToken_isRevisionIdentityAlias();
};

void IdentitySeparationTest::persistedArtifactIdentity_hasStorageFields()
{
    pdf::PDFArtifactIdentity identity;
    QVERIFY(!identity.isValid());

    identity.sha256 = QString(64, QLatin1Char('a'));
    identity.size = 1024;
    identity.logicalName = QStringLiteral("report.pdf");
    identity.storageToken = QStringLiteral("token-1");

    QCOMPARE(identity.mediaType, QStringLiteral("application/pdf"));
    QVERIFY(identity.isValid());

    // Round-trips through its persisted JSON form.
    const pdf::PDFArtifactIdentity restored = pdf::PDFArtifactIdentity::fromJson(identity.toJson());
    QCOMPARE(restored.sha256, identity.sha256);
    QCOMPARE(restored.size, identity.size);
    QCOMPARE(restored.logicalName, identity.logicalName);
    QCOMPARE(restored.storageToken, identity.storageToken);
}

void IdentitySeparationTest::documentIdentity_hasInSessionFields()
{
    pdf::PDFDocumentIdentity identity;
    QVERIFY(!identity.isValid());

    identity.documentId = QStringLiteral("7f3a");
    QVERIFY(identity.isValid());

    pdf::PDFDocumentIdentity other;
    other.sourceDataHash = QByteArrayLiteral("hash");
    QVERIFY(other.isValid());
    QVERIFY(!(identity == other));
}

void IdentitySeparationTest::documentIdentity_fromNullDocumentIsInvalid()
{
    const pdf::PDFDocumentIdentity identity = pdf::PDFDocumentIdentity::fromDocument(nullptr);
    QVERIFY(!identity.isValid());
    QVERIFY(identity.documentId.isEmpty());
    QVERIFY(identity.sourceDataHash.isEmpty());
}

void IdentitySeparationTest::revisionIdentity_carriesDocumentIdentity()
{
    pdf::PDFRevisionIdentity revision;
    QVERIFY(!revision.isValid());

    revision.document.documentId = QStringLiteral("7f3a");
    QVERIFY(revision.isValid());
    QVERIFY(revision.toString().contains(QStringLiteral("7f3a")));

    pdf::PDFRevisionIdentity later = revision;
    later.documentRevision = 1;
    QVERIFY(revision < later);
    QVERIFY(!(revision == later));
}

void IdentitySeparationTest::revisionToken_isRevisionIdentityAlias()
{
    pdf::PDFRevisionToken token;
    token.document.documentId = QStringLiteral("7f3a");
    token.documentRevision = 3;
    token.cacheGeneration = 4;
    token.effectiveProfileIdentity = QStringLiteral("profile-a");

    const pdf::PDFRevisionIdentity identity = token;
    QCOMPARE(identity.document.documentId, token.document.documentId);
    QCOMPARE(identity.documentRevision, token.documentRevision);
    QCOMPARE(identity.cacheGeneration, token.cacheGeneration);
    QCOMPARE(identity.effectiveProfileIdentity, token.effectiveProfileIdentity);
    QVERIFY(identity == token);
}

QTEST_GUILESS_MAIN(IdentitySeparationTest)

#include "tst_identityseparationtest.moc"
