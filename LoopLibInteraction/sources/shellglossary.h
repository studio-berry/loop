#pragma once

#include <QCoreApplication>
#include <QString>

#include <utility>

namespace pdfinteraction::glossary
{

inline QString term(const QString& code)
{
    constexpr std::pair<const char*, const char*> terms[] = {
        { "bleed", QT_TRANSLATE_NOOP("LoopGlossary", "Bleed") },
        { "bleed-margin-empty", QT_TRANSLATE_NOOP("LoopGlossary", "Bleed margin empty") },
        { "budget-exceeded", QT_TRANSLATE_NOOP("LoopGlossary", "Budget exceeded") },
        { "check-error", QT_TRANSLATE_NOOP("LoopGlossary", "Check error") },
        { "check-incomplete", QT_TRANSLATE_NOOP("LoopGlossary", "Check incomplete") },
        { "check-unsupported", QT_TRANSLATE_NOOP("LoopGlossary", "Check unsupported") },
        { "color-inventory", QT_TRANSLATE_NOOP("LoopGlossary", "Color inventory") },
        { "color-mode", QT_TRANSLATE_NOOP("LoopGlossary", "Color mode") },
        { "conformance-claims", QT_TRANSLATE_NOOP("LoopGlossary", "Conformance claims") },
        { "content-bleed", QT_TRANSLATE_NOOP("LoopGlossary", "Content bleed") },
        { "dieline", QT_TRANSLATE_NOOP("LoopGlossary", "Dieline") },
        { "dieline-missing", QT_TRANSLATE_NOOP("LoopGlossary", "Dieline missing") },
        { "dieline-printing", QT_TRANSLATE_NOOP("LoopGlossary", "Dieline printing") },
        { "embedded-fonts", QT_TRANSLATE_NOOP("LoopGlossary", "Embedded fonts") },
        { "error", QT_TRANSLATE_NOOP("LoopGlossary", "Error") },
        { "evidence-incomplete", QT_TRANSLATE_NOOP("LoopGlossary", "Evidence incomplete") },
        { "fail", QT_TRANSLATE_NOOP("LoopGlossary", "Fail") },
        { "font-integrity", QT_TRANSLATE_NOOP("LoopGlossary", "Font integrity") },
        { "hairline-stroke", QT_TRANSLATE_NOOP("LoopGlossary", "Hairline stroke") },
        { "hidden-layers", QT_TRANSLATE_NOOP("LoopGlossary", "Hidden layers") },
        { "image-resolution", QT_TRANSLATE_NOOP("LoopGlossary", "Image resolution") },
        { "incomplete", QT_TRANSLATE_NOOP("LoopGlossary", "Incomplete") },
        { "info", QT_TRANSLATE_NOOP("LoopGlossary", "Info") },
        { "ink-coverage", QT_TRANSLATE_NOOP("LoopGlossary", "Ink coverage") },
        { "interactive-thread-violation", QT_TRANSLATE_NOOP("LoopGlossary", "Interactive thread violation") },
        { "invisible-content", QT_TRANSLATE_NOOP("LoopGlossary", "Invisible content") },
        { "needs-auto-bleed", QT_TRANSLATE_NOOP("LoopGlossary", "Needs auto bleed") },
        { "not-checked", QT_TRANSLATE_NOOP("LoopGlossary", "Not checked") },
        { "obscured-content", QT_TRANSLATE_NOOP("LoopGlossary", "Obscured content") },
        { "off-page-content", QT_TRANSLATE_NOOP("LoopGlossary", "Off page content") },
        { "output-intent", QT_TRANSLATE_NOOP("LoopGlossary", "Output intent") },
        { "output-intent-ambiguous", QT_TRANSLATE_NOOP("LoopGlossary", "Output intent ambiguous") },
        { "output-intent-color-mismatch", QT_TRANSLATE_NOOP("LoopGlossary", "Output intent color mismatch") },
        { "output-intent-conflict", QT_TRANSLATE_NOOP("LoopGlossary", "Output intent conflict") },
        { "output-intent-identity", QT_TRANSLATE_NOOP("LoopGlossary", "Output intent identity") },
        { "output-intent-malformed", QT_TRANSLATE_NOOP("LoopGlossary", "Output intent malformed") },
        { "output-intent-missing", QT_TRANSLATE_NOOP("LoopGlossary", "Output intent missing") },
        { "output-intent-profile-identity", QT_TRANSLATE_NOOP("LoopGlossary", "Output intent profile identity") },
        { "output-intent-profile-invalid", QT_TRANSLATE_NOOP("LoopGlossary", "Output intent profile invalid") },
        { "output-intent-profile-missing", QT_TRANSLATE_NOOP("LoopGlossary", "Output intent profile missing") },
        { "output-intent-subtype", QT_TRANSLATE_NOOP("LoopGlossary", "Output intent subtype") },
        { "page-size", QT_TRANSLATE_NOOP("LoopGlossary", "Page size") },
        { "pass", QT_TRANSLATE_NOOP("LoopGlossary", "Pass") },
        { "passed", QT_TRANSLATE_NOOP("LoopGlossary", "Passed") },
        { "pdfx-rule-failed", QT_TRANSLATE_NOOP("LoopGlossary", "PDF/X rule failed") },
        { "pdfx-rule-incomplete", QT_TRANSLATE_NOOP("LoopGlossary", "PDF/X rule incomplete") },
        { "processing-step-missing", QT_TRANSLATE_NOOP("LoopGlossary", "Processing step missing") },
        { "processing-steps", QT_TRANSLATE_NOOP("LoopGlossary", "Processing steps") },
        { "profile", QT_TRANSLATE_NOOP("LoopGlossary", "Profile") },
        { "rich-black", QT_TRANSLATE_NOOP("LoopGlossary", "Rich black") },
        { "separation", QT_TRANSLATE_NOOP("LoopGlossary", "Separation") },
        { "spot-color", QT_TRANSLATE_NOOP("LoopGlossary", "Spot color") },
        { "thin-annotation", QT_TRANSLATE_NOOP("LoopGlossary", "Thin annotation") },
        { "thin-clipped-part", QT_TRANSLATE_NOOP("LoopGlossary", "Thin clipped part") },
        { "thin-fill", QT_TRANSLATE_NOOP("LoopGlossary", "Thin fill") },
        { "thin-negative-space", QT_TRANSLATE_NOOP("LoopGlossary", "Thin negative space") },
        { "thin-parts", QT_TRANSLATE_NOOP("LoopGlossary", "Thin parts") },
        { "thin-stroke", QT_TRANSLATE_NOOP("LoopGlossary", "Thin stroke") },
        { "thin-strokes", QT_TRANSLATE_NOOP("LoopGlossary", "Thin strokes") },
        { "transparency-blend-mode", QT_TRANSLATE_NOOP("LoopGlossary", "Transparency blend mode") },
        { "transparency-blend-space", QT_TRANSLATE_NOOP("LoopGlossary", "Transparency blend space") },
        { "transparency-overprint-interaction", QT_TRANSLATE_NOOP("LoopGlossary", "Transparency overprint interaction") },
        { "transparency-risk", QT_TRANSLATE_NOOP("LoopGlossary", "Transparency risk") },
        { "trim", QT_TRANSLATE_NOOP("LoopGlossary", "Trim") },
        { "waived", QT_TRANSLATE_NOOP("LoopGlossary", "Waived") },
        { "warning", QT_TRANSLATE_NOOP("LoopGlossary", "Warning") },
        { "white-overprint", QT_TRANSLATE_NOOP("LoopGlossary", "White overprint") },
    };
    for (const auto& [key, source] : terms)
    {
        if (code == QLatin1String(key))
        {
            return QCoreApplication::translate("LoopGlossary", source);
        }
    }
    return code;
}

}   // namespace pdfinteraction::glossary
