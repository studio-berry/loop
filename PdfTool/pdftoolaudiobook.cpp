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

#include "pdftoolaudiobook.h"

#ifdef Q_OS_WIN

#include <QFileInfo>

#include <windows.h>
#include <sapi.h>

#if defined(PDF4QT_USE_PRAGMA_LIB)
#pragma comment(lib, "ole32")
#endif

namespace pdftool
{

static PDFToolAudioBook s_audioBookApplication;
static PDFToolAudioBookVoices s_audioBookVoicesApplication;

PDFVoiceInfo::PDFVoiceInfo(std::map<QString, QString> properties, ISpObjectToken* voiceToken) :
    m_properties(qMove(properties)),
    m_voiceToken(voiceToken)
{
    if (m_voiceToken)
    {
        m_voiceToken->AddRef();
    }
}

PDFVoiceInfo::PDFVoiceInfo(PDFVoiceInfo&& other)
{
    std::swap(m_properties, other.m_properties);
    std::swap(m_voiceToken, other.m_voiceToken);
}

PDFVoiceInfo& PDFVoiceInfo::operator=(PDFVoiceInfo&& other)
{
    std::swap(m_properties, other.m_properties);
    std::swap(m_voiceToken, other.m_voiceToken);
    return *this;
}

PDFVoiceInfo::~PDFVoiceInfo()
{
    if (m_voiceToken)
    {
        m_voiceToken->Release();
    }
}

QLocale PDFVoiceInfo::getLocale() const
{
    bool ok = false;
    LCID locale = getLanguage().toInt(&ok, 16);

    if (ok)
    {
        // Language name
        int count = GetLocaleInfoW(locale, LOCALE_SISO639LANGNAME, NULL, 0);
        std::vector<wchar_t> buffer(count, wchar_t());
        GetLocaleInfoW(locale, LOCALE_SISO639LANGNAME, buffer.data(), int(buffer.size()));
        QString languageCode = QString::fromWCharArray(buffer.data());

        // Country name
        count = GetLocaleInfoW(locale, LOCALE_SISO3166CTRYNAME, NULL, 0);
        buffer.resize(count, wchar_t());
        GetLocaleInfoW(locale, LOCALE_SISO3166CTRYNAME, buffer.data(), int(buffer.size()));
        QString countryCode = QString::fromWCharArray(buffer.data());

        return QLocale(QString("%1_%2").arg(languageCode, countryCode));
    }

    return QLocale();
}

QString PDFVoiceInfo::getStringValue(QString key) const
{
    auto it = m_properties.find(key);
    if (it != m_properties.cend())
    {
        return it->second;
    }

    return QString();
}

PDFToolExitCode PDFToolAudioBookBase::fillVoices(const PDFToolOptions& options, PDFVoiceInfoList& list, bool fillVoiceTokenPointers)
{
    PDFToolExitCode result = PDFToolExitCode::Success;

    QStringList voiceSelector;
    if (!options.textVoiceName.isEmpty())
    {
        voiceSelector << QString("Name=%1").arg(options.textVoiceName);
    }
    if (!options.textVoiceGender.isEmpty())
    {
        voiceSelector << QString("Gender=%1").arg(options.textVoiceGender);
    }
    if (!options.textVoiceAge.isEmpty())
    {
        voiceSelector << QString("Age=%1").arg(options.textVoiceAge);
    }
    if (!options.textVoiceLangCode.isEmpty())
    {
        voiceSelector << QString("Language=%1").arg(options.textVoiceLangCode);
    }
    QString voiceSelectorString = voiceSelector.join(";");
    LPCWSTR requiredAttributes = !voiceSelectorString.isEmpty() ? (LPCWSTR)voiceSelectorString.utf16() : nullptr;

    ISpObjectTokenCategory* category = nullptr;
    if (!SUCCEEDED(::CoCreateInstance(CLSID_SpObjectTokenCategory, NULL, CLSCTX_ALL, __uuidof(ISpObjectTokenCategory), (LPVOID*)&category)))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("audio.voice-enumeration-failed"), PDFToolTranslationContext::tr("SAPI Error: Cannot enumerate SAPI voices."));
        return PDFToolExitCode::ProcessingFailure;
    }

    if (!SUCCEEDED(category->SetId(SPCAT_VOICES, FALSE)))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("audio.voice-enumeration-failed"), PDFToolTranslationContext::tr("SAPI Error: Cannot enumerate SAPI voices."));
        category->Release();
        return PDFToolExitCode::ProcessingFailure;
    }

    IEnumSpObjectTokens* enumTokensObject = nullptr;
    if (SUCCEEDED(category->EnumTokens(requiredAttributes, NULL, &enumTokensObject)))
    {
        ISpObjectToken* token = nullptr;
        while (SUCCEEDED(enumTokensObject->Next(1, &token, NULL)))
        {
            if (token)
            {
                /* Attributes can be for example:
                 *    Version,
                 *    Language,
                 *    Gender,
                 *    Age,
                 *    Name
                 *    Vendor */

                std::map<QString, QString> properties;

                ISpDataKey* attributes = nullptr;
                if (SUCCEEDED(token->OpenKey(L"Attributes", &attributes)))
                {
                    for (ULONG i = 0; ; ++i)
                    {
                        LPWSTR valueName = NULL;
                        if (SUCCEEDED(attributes->EnumValues(i, &valueName)))
                        {
                            LPWSTR data = NULL;
                            if (SUCCEEDED(attributes->GetStringValue(valueName, &data)))
                            {
                                QString propertyName = QString::fromWCharArray(valueName);
                                QString propertyValue = QString::fromWCharArray(data);
                                if (!propertyValue.isEmpty())
                                {
                                    properties[propertyName] = propertyValue;
                                }
                                ::CoTaskMemFree(data);
                            }

                            ::CoTaskMemFree(valueName);
                        }
                        else
                        {
                            break;
                        }
                    }
                    attributes->Release();
                }

                if (fillVoiceTokenPointers)
                {
                    list.emplace_back(qMove(properties), token);
                }
                else
                {
                    list.emplace_back(qMove(properties), nullptr);
                }

                token->Release();
            }
            else
            {
                break;
            }
        }
    }
    else
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("audio.voice-enumeration-failed"), PDFToolTranslationContext::tr("SAPI Error: Cannot enumerate SAPI voices."));
        result = PDFToolExitCode::ProcessingFailure;
    }

    if (enumTokensObject)
    {
        enumTokensObject->Release();
    }

    if (category)
    {
        category->Release();
    }

    return result;
}

PDFToolExitCode PDFToolAudioBookBase::showVoiceList(const PDFToolOptions& options)
{
    PDFVoiceInfoList voices;
    PDFToolExitCode result = fillVoices(options, voices, false);

    PDFOutputFormatter formatter(options.outputStyle);
    formatter.beginDocument("voices", PDFToolTranslationContext::tr("Available voices for given settings:"));
    formatter.endl();

    formatter.beginTable("voices", PDFToolTranslationContext::tr("Voice list"));

    formatter.beginTableHeaderRow("header");
    formatter.writeTableHeaderColumn("name", PDFToolTranslationContext::tr("Name"), Qt::AlignLeft);
    formatter.writeTableHeaderColumn("gender", PDFToolTranslationContext::tr("Gender"), Qt::AlignLeft);
    formatter.writeTableHeaderColumn("age", PDFToolTranslationContext::tr("Age"), Qt::AlignLeft);
    formatter.writeTableHeaderColumn("language-code", PDFToolTranslationContext::tr("Lang. Code"), Qt::AlignLeft);
    formatter.writeTableHeaderColumn("locale", PDFToolTranslationContext::tr("Locale"), Qt::AlignLeft);
    formatter.writeTableHeaderColumn("language", PDFToolTranslationContext::tr("Language"), Qt::AlignLeft);
    formatter.writeTableHeaderColumn("country", PDFToolTranslationContext::tr("Country"), Qt::AlignLeft);
    formatter.writeTableHeaderColumn("vendor", PDFToolTranslationContext::tr("Vendor"), Qt::AlignLeft);
    formatter.writeTableHeaderColumn("version", PDFToolTranslationContext::tr("Version"), Qt::AlignLeft);
    formatter.endTableHeaderRow();

    for (const PDFVoiceInfo& voice : voices)
    {
        QLocale locale = voice.getLocale();
        formatter.beginTableRow("voice");
        formatter.writeTableColumn("name", voice.getName(), Qt::AlignLeft);
        formatter.writeTableColumn("gender", voice.getGender(), Qt::AlignLeft);
        formatter.writeTableColumn("age", voice.getAge(), Qt::AlignLeft);
        formatter.writeTableColumn("language", voice.getLanguage(), Qt::AlignLeft);
        formatter.writeTableColumn("locale", locale.name(), Qt::AlignLeft);
        formatter.writeTableColumn("language", locale.nativeLanguageName(), Qt::AlignLeft);
        formatter.writeTableColumn("country", locale.nativeTerritoryName(), Qt::AlignLeft);
        formatter.writeTableColumn("vendor", voice.getVendor(), Qt::AlignLeft);
        formatter.writeTableColumn("version", voice.getVersion(), Qt::AlignLeft);
        formatter.endTableRow();
    }

    formatter.endTable();

    formatter.endDocument();
    if (options.outputStyle == PDFOutputFormatter::Style::Json)
    {
        if (options.executionContext)
        {
            options.executionContext->setData(formatter.getJsonObject());
        }
    }
    else
    {
        PDFConsole::writeText(formatter.getString(), options.outputCodec);
    }

    return result;
}

QString PDFToolAudioBookVoices::getStandardString(PDFToolAbstractApplication::StandardString standardString) const
{
    switch (standardString)
    {
        case Command:
            return "audio-book-voices";

        case Name:
            return PDFToolTranslationContext::tr("Audio book voices");

        case Description:
            return PDFToolTranslationContext::tr("List of available voices for audio book conversion.");

        default:
            Q_ASSERT(false);
            break;
    }

    return QString();
}

PDFToolExitCode PDFToolAudioBookVoices::execute(const PDFToolOptions& options)
{
    if (!SUCCEEDED(::CoInitialize(nullptr)))
    {
        return PDFToolExitCode::ProcessingFailure;
    }

    PDFToolExitCode returnCode = showVoiceList(options);

    ::CoUninitialize();

    return returnCode;
}

PDFToolAbstractApplication::Options PDFToolAudioBookVoices::getOptionsFlags() const
{
    return ConsoleFormat | VoiceSelector;
}

QString PDFToolAudioBook::getStandardString(StandardString standardString) const
{
    switch (standardString)
    {
        case Command:
            return "audio-book";

        case Name:
            return PDFToolTranslationContext::tr("Audio book convertor");

        case Description:
            return PDFToolTranslationContext::tr("Convert your document to a simple audio book.");

        default:
            Q_ASSERT(false);
            break;
    }

    return QString();
}


PDFToolExitCode PDFToolAudioBook::getDocumentTextFlow(const PDFToolOptions& options, pdf::PDFDocumentTextFlow& flow)
{
    pdf::PDFDocument document;
    QByteArray sourceData;
    if (!readDocument(options, document, &sourceData, false))
    {
        return PDFToolExitCode::InputError;
    }

    QString parseError;
    std::vector<pdf::PDFInteger> pages = options.getPageRange(document.getCatalog()->getPageCount(), parseError, true);

    if (!parseError.isEmpty())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("cli.invalid-arguments"), parseError);
        return PDFToolExitCode::InvalidInvocation;
    }

    pdf::PDFDocumentTextFlowFactory factory;
    flow = factory.create(&document, pages, options.textAnalysisAlgorithm);

    return PDFToolExitCode::Success;
}

PDFToolExitCode PDFToolAudioBook::createAudioBook(const PDFToolOptions& options, pdf::PDFDocumentTextFlow& flow)
{
    QString audioString;
    QTextStream textStream(&audioString);

    for (const pdf::PDFDocumentTextFlow::Item& item : flow.getItems())
    {
        if (item.flags.testFlag(pdf::PDFDocumentTextFlow::PageStart) && options.textSpeechMarkPageNumbers)
        {
            textStream << QString("<bookmark mark=\"%1\"/>").arg(item.text) << Qt::endl;
        }

        if (!item.text.isEmpty())
        {
            bool showText = (item.flags.testFlag(pdf::PDFDocumentTextFlow::Text)) ||
                            (item.flags.testFlag(pdf::PDFDocumentTextFlow::PageStart) && options.textSpeechSayPageNumbers) ||
                            (item.flags.testFlag(pdf::PDFDocumentTextFlow::PageEnd) && options.textSpeechSayPageNumbers) ||
                            (item.flags.testFlag(pdf::PDFDocumentTextFlow::StructureTitle) && options.textSpeechSayStructTitles) ||
                            (item.flags.testFlag(pdf::PDFDocumentTextFlow::StructureAlternativeDescription) && options.textSpeechSayStructAlternativeDescription) ||
                            (item.flags.testFlag(pdf::PDFDocumentTextFlow::StructureExpandedForm) && options.textSpeechSayStructExpandedForm) ||
                            (item.flags.testFlag(pdf::PDFDocumentTextFlow::StructureActualText) && options.textSpeechSayStructActualText);

            if (showText)
            {
                textStream << item.text << Qt::endl;
            }
        }
    }

    PDFVoiceInfoList voices;
    fillVoices(options, voices, true);

    // Do we have any voice?
    if (voices.empty())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("audio.voice-not-found"), PDFToolTranslationContext::tr("No suitable voice found."));
        return PDFToolExitCode::ProcessingFailure;
    }

    if (!voices.front().getVoiceToken())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("audio.invalid-voice"), PDFToolTranslationContext::tr("Invalid voice."));
        return PDFToolExitCode::ProcessingFailure;
    }

    QFileInfo info(options.document);
    QString outputFile = QString("%1/%2.%3").arg(info.path(), info.completeBaseName(), options.textSpeechAudioFormat);
    BSTR outputFileName = (BSTR)outputFile.utf16();

    ISpeechFileStream* stream = nullptr;
    if (!SUCCEEDED(::CoCreateInstance(CLSID_SpFileStream, NULL, CLSCTX_ALL, __uuidof(ISpeechFileStream), (LPVOID*)&stream)))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("output.write-failed"), PDFToolTranslationContext::tr("Cannot create output stream '%1'.").arg(outputFile), QJsonObject{{QStringLiteral("path"), outputFile}});
        return PDFToolExitCode::ProcessingFailure;
    }

    ISpVoice* voice = nullptr;
    if (!SUCCEEDED(::CoCreateInstance(CLSID_SpVoice, NULL, CLSCTX_ALL, __uuidof(ISpVoice), (LPVOID*)&voice)))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("audio.voice-create-failed"), PDFToolTranslationContext::tr("Cannot create voice."));
        stream->Release();
        return PDFToolExitCode::ProcessingFailure;
    }

    if (!SUCCEEDED(stream->Open(outputFileName, SSFMCreateForWrite)))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("output.write-failed"), PDFToolTranslationContext::tr("Cannot create output stream '%1'.").arg(outputFile), QJsonObject{{QStringLiteral("path"), outputFile}});
        voice->Release();
        stream->Release();
        return PDFToolExitCode::ProcessingFailure;
    }

    ISpObjectToken* voiceToken = voices.front().getVoiceToken();
    if (!SUCCEEDED(voice->SetVoice(voiceToken)))
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Warning, QStringLiteral("audio.voice-fallback"), PDFToolTranslationContext::tr("Failed to set requested voice. Default voice will be used."));
    }
    voices.clear();

    LPCWSTR stringToSpeak = (LPCWSTR)audioString.utf16();

    voice->SetOutput(stream, FALSE);
    voice->Speak(stringToSpeak, SPF_PURGEBEFORESPEAK | SPF_PARSE_SAPI, NULL);

    voice->Release();
    stream->Release();

    return PDFToolExitCode::Success;
}

PDFToolExitCode PDFToolAudioBook::execute(const PDFToolOptions& options)
{
    pdf::PDFDocumentTextFlow textFlow;
    PDFToolExitCode result = getDocumentTextFlow(options, textFlow);
    if (result != PDFToolExitCode::Success)
    {
        return result;
    }

    if (textFlow.isEmpty())
    {
        reportDiagnostic(options, PDFToolDiagnosticSeverity::Error, QStringLiteral("audio.no-text"), PDFToolTranslationContext::tr("No text extracted to be converted to audio book."));
        return PDFToolExitCode::ProcessingFailure;
    }

    auto comResult = ::CoInitialize(nullptr);
    if (!SUCCEEDED(comResult))
    {
        return PDFToolExitCode::ProcessingFailure;
    }

    result = createAudioBook(options, textFlow);

    ::CoUninitialize();

    return result;
}

PDFToolAbstractApplication::Options PDFToolAudioBook::getOptionsFlags() const
{
    return ConsoleFormat | OpenDocument | PageSelector | VoiceSelector | TextAnalysis | TextSpeech;
}

}   // namespace pdftool

#endif
