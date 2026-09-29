#include "operator_factory.h"
#include "operator_base.h"
#include "color_segment.h"
#include "blob_count.h"
#include "count_check.h"
#include "template_match.h"
#include "qr_decode.h"
#include "image_source_dir.h"
#include "image_source_camera.h"
#include "image_source_video.h"
#include "pcb_inspect.h"
#include "pcb_defect_judge.h"
#include "dl_infer.h"
#include "line_fit.h"
#include "circle_fit.h"
#include "caliper_dist.h"

namespace w7 {

OperatorBase* createOperatorByName(const QString& name) {
    if (name == "color_segment")      return new ColorSegment();
    if (name == "blob_count")         return new BlobCount();
    if (name == "count_check")        return new CountCheck();
    if (name == "template_match")     return new TemplateMatch();
    if (name == "qr_decode")          return new QRDecode();
    if (name == "image_source_dir")  return new ImageSourceDir();
    if (name == "image_source_camera") return new ImageSourceCamera();
    if (name == "image_source_video")  return new ImageSourceVideo();
    if (name == "pcb_inspect")        return new PcbInspect();
    if (name == "pcb_defect_judge")   return new PcbDefectJudge();
    if (name == "dl_infer")           return new DlInfer();
    if (name == "line_fit")           return new LineFit();
    if (name == "circle_fit")         return new CircleFit();
    if (name == "caliper_dist")       return new CaliperDist();
    return nullptr;
}

} // namespace w7
