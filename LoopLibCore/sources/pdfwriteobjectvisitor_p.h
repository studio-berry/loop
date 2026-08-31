// MIT License
#ifndef PDFWRITEOBJECTVISITOR_P_H
#define PDFWRITEOBJECTVISITOR_P_H

#include "pdfoperationcontrol.h"
#include "pdfvisitor.h"

#include <QIODevice>

namespace pdf
{

/// Private serializer for one PDF object tree. Keeping lexical emission here
/// leaves PDFDocumentWriter responsible for document/xref orchestration.
class PDFWriteObjectVisitor final : public PDFAbstractVisitor
{
public:
    explicit PDFWriteObjectVisitor(QIODevice* device,
                                   const PDFOperationControl* operationControl = nullptr) :
        m_device(device),
        m_operationControl(operationControl)
    {
    }

    void visitNull() override;
    void visitBool(bool value) override;
    void visitInt(PDFInteger value) override;
    void visitReal(PDFReal value) override;
    void visitString(PDFStringRef string) override;
    void visitName(PDFStringRef name) override;
    void visitArray(const PDFArray* array) override;
    void visitDictionary(const PDFDictionary* dictionary) override;
    void visitStream(const PDFStream* stream) override;
    void visitReference(PDFObjectReference reference) override;

    bool isCancelled() const noexcept { return m_cancelled; }

private:
    bool checkpoint() noexcept
    {
        if (m_cancelled || PDFOperationControl::isOperationCancelled(m_operationControl))
        {
            m_cancelled = true;
            return true;
        }
        return false;
    }

    void writeName(const QByteArray& string);

    QIODevice* m_device = nullptr;
    const PDFOperationControl* m_operationControl = nullptr;
    bool m_cancelled = false;
};

}   // namespace pdf

#endif   // PDFWRITEOBJECTVISITOR_P_H
