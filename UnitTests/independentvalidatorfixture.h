#pragma once

#include "pdfstandardconversion.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

namespace independent_test
{
inline QString writeVeraPdfScript(const QTemporaryDir& directory, const QString& compliant, const QString& profile, bool mutate)
{
#ifdef Q_OS_WIN
    const QString path = directory.filePath(QStringLiteral("verapdf.bat"));
    const QString name = QStringLiteral("%~5");
#else
    const QString path = directory.filePath(QStringLiteral("verapdf"));
    const QString name = QStringLiteral("%s");
#endif
    const QString compliantCount = compliant == QLatin1String("false") ? QStringLiteral("0") : QStringLiteral("1");
    const QString nonCompliantCount = compliant == QLatin1String("false") ? QStringLiteral("1") : QStringLiteral("0");
    QString xml = QStringLiteral("<report><jobs><job><item><name>%1</name></item><validationReport profileName=\"%2\" isCompliant=\"%3\" jobEndStatus=\"normal\"><details passedRules=\"102\" failedRules=\"0\" passedChecks=\"504\" failedChecks=\"0\"/></validationReport></job></jobs><batchSummary totalJobs=\"1\" failedToParse=\"0\" encrypted=\"0\" outOfMemory=\"0\" veraExceptions=\"0\"><validationReports compliant=\"%4\" nonCompliant=\"%5\" failedJobs=\"0\">1</validationReports></batchSummary></report>")
                      .arg(name, profile, compliant, compliantCount, nonCompliantCount);
#ifdef Q_OS_WIN
    xml.replace(QLatin1Char('<'), QStringLiteral("^<"));
    xml.replace(QLatin1Char('>'), QStringLiteral("^>"));
    QString body = QStringLiteral("@echo off\r\nif \"%~1\"==\"--version\" (\r\necho veraPDF 1.28.2\r\nexit /b 0\r\n)\r\necho %1\r\n").arg(xml);
    if (mutate)
        body += QStringLiteral("echo changed>\"%~5\"\r\n");
#else
    QString body = QStringLiteral("#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'veraPDF 1.28.2'; exit 0; fi\nprintf '%1\\n' \"$5\"\n").arg(xml);
    if (mutate)
        body += QStringLiteral("printf changed > \"$5\"\n");
#endif
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(body.toUtf8());
    file.close();
#ifndef Q_OS_WIN
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
#endif
    return path;
}

inline pdf::PDFStandardConversionSettings settings(const QString& program)
{
    pdf::PDFStandardConversionSettings settings;
    settings.target = pdf::PDFStandardTarget::PDFA2b;
    settings.independentValidatorProgram = program;
    QFile profile(QFINDTESTDATA("testdata/synthetic-cmyk.icc"));
    if (profile.open(QIODevice::ReadOnly))
        settings.outputIntentIccData = profile.readAll();
    return settings;
}

inline QJsonObject parameters(const pdf::PDFStandardConversionSettings& settings)
{
    return QJsonObject{ { QStringLiteral("target"), pdf::pdfStandardTargetToString(settings.target) },
                        { QStringLiteral("target_icc_base64"), QString::fromLatin1(settings.outputIntentIccData.toBase64()) },
                        { QStringLiteral("validator_program"), settings.independentValidatorProgram },
                        { QStringLiteral("validation_contract"), 2 } };
}
}
