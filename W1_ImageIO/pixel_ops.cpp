#include "pixel_ops.h"

namespace w1 {

cv::Mat makeEx3Image() {
    cv::Mat img(300, 400, CV_8UC3, cv::Scalar(255, 255, 255));
    img(cv::Rect(0, 0, 200, 300)).setTo(cv::Scalar(200, 100, 100));  // 浅蓝(B=200)
    img(cv::Rect(200, 100, 200, 100)).setTo(cv::Scalar(220, 80, 80)); // 深蓝带
    return img;
}

bool isBluePixel(const cv::Vec3b& px) {
    // BGR 顺序：px[0]=蓝, px[1]=绿, px[2]=红
    return px[0] > 100 && px[1] < 150 && px[2] < 150;
}

int paintBlueToBlack(cv::Mat& img) {
    int blackCount = 0;
    for (int y = 0; y < img.rows; ++y) {
        for (int x = 0; x < img.cols; ++x) {
            cv::Vec3b& px = img.at<cv::Vec3b>(y, x);
            if (isBluePixel(px)) {
                px = cv::Vec3b(0, 0, 0);
                ++blackCount;
            }
        }
    }
    return blackCount;
}

}  // namespace w1
