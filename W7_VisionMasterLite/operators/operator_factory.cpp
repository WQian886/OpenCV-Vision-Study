#include "operator_factory.h"
#include "operator_base.h"
#include "color_segment.h"
#include "blob_count.h"
#include "count_check.h"
#include "template_match.h"
#include "qr_decode.h"

namespace w7 {

OperatorBase* createOperatorByName(const QString& name) {
    if (name == "color_segment")  return new ColorSegment();
    if (name == "blob_count")     return new BlobCount();
    if (name == "count_check")    return new CountCheck();
    if (name == "template_match") return new TemplateMatch();
    if (name == "qr_decode")      return new QRDecode();
    return nullptr;
}

} // namespace w7
