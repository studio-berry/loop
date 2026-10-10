// MIT License
#ifndef QUICKOUTPUT_H
#define QUICKOUTPUT_H

#include "pdfdocument.h"
#include "pdfjobscheduler.h"
#include <QJsonObject>
#include <QTemporaryDir>
#include <memory>

namespace loopeditor
{
enum class OutputFormat
{
    PublishedPdf,
    PrintPdf,
    Png,
    Tiff
};

struct OutputRequest
{
    OutputFormat format = OutputFormat::PublishedPdf;
    pdf::PDFDocumentPointer document;
    QString sourcePath;
    QByteArray publishedBytes;
    QJsonObject publicationReceipt;
    QString revision;
    int page = 0;
    int dpi = 300;
    bool fidelityAcknowledged = false;
};

struct OutputResult
{
    std::shared_ptr<QTemporaryDir> staging;
    QString stagedPath;
    QString error;
    QJsonObject record;
};

void prepareOutput(const OutputRequest& request, OutputResult& result, pdf::PDFJobContext& job);
}
#endif
