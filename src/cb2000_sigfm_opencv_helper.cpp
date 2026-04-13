/*
 * CanvasBio CB2000 OpenCV helper
 *
 * Copyright (C) 2025-2026 Kristofer Pagnussat
 *
 * This file is distributed under the terms of the GNU Lesser General Public
 * License version 2.1 or, at your option, any later version.
 *
 * Portions of the helper logic are derived/adapted from the SIGFM reference
 * implementation by Goodix Fingerprint Linux Development under the MIT
 * License. The preserved upstream notice is available in:
 *   - THIRD_PARTY_NOTICES.md
 *   - third_party_licenses/SIGFM-MIT.txt
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <vector>

#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>

extern "C" {

typedef struct {
    double ratio_test;
    double length_match;
    double angle_match;
    unsigned int min_matches;
} Cb2000SigfmCvConfig;

typedef struct {
    unsigned int probe_keypoints;
    unsigned int gallery_keypoints;
    unsigned int raw_matches;
    unsigned int unique_matches;
    unsigned int angle_pairs;
    unsigned int consensus_pairs;
    double inlier_ratio;
    double shift_dx;
    double shift_dy;
    unsigned int original_match;
    double score;
} Cb2000SigfmCvTelemetry;

/* R2.4: feature mosaicking keypoint (must match Cb2000MosaicKeypoint in matcher.h) */
#define CB2000_MOSAIC_DESC_LEN 128

typedef struct {
    float x;
    float y;
    float angle; /* SIFT orientation, degrees */
    float desc[CB2000_MOSAIC_DESC_LEN];
} Cb2000MosaicKeypoint;

int cb2000_sigfm_opencv_pair_match(const uint8_t              *probe,
                                   const uint8_t              *gallery,
                                   int                         width,
                                   int                         height,
                                   const Cb2000SigfmCvConfig  *cfg,
                                   Cb2000SigfmCvTelemetry     *tel);

int cb2000_sigfm_opencv_build_mosaic(const uint8_t * const     *images,
                                     int                        n_images,
                                     int                        width,
                                     int                        height,
                                     const Cb2000SigfmCvConfig *cfg,
                                     Cb2000MosaicKeypoint      *out_kp,
                                     int                        max_kp);

int cb2000_sigfm_opencv_match_mosaic(const uint8_t             *probe,
                                     int                        width,
                                     int                        height,
                                     const Cb2000MosaicKeypoint *mosaic_kp,
                                     int                        mosaic_count,
                                     const Cb2000SigfmCvConfig *cfg,
                                     Cb2000SigfmCvTelemetry    *tel);
}

namespace {

struct CorrMatch {
    cv::Point2i p1;
    cv::Point2i p2;

    bool operator<(const CorrMatch &other) const
    {
        if (p1.y != other.p1.y)
            return p1.y < other.p1.y;
        if (p1.x != other.p1.x)
            return p1.x < other.p1.x;
        if (p2.y != other.p2.y)
            return p2.y < other.p2.y;
        return p2.x < other.p2.x;
    }
};

struct AngleRec {
    double cos_v;
    double sin_v;
};

static inline double
clampd(double v, double lo, double hi)
{
    return std::max(lo, std::min(hi, v));
}

static inline bool
ratio_close(double a, double b, double tolerance)
{
    const double aa = std::abs(a);
    const double bb = std::abs(b);
    const double lo = std::min(aa, bb);
    const double hi = std::max(aa, bb);
    if (hi <= 1e-9)
        return true;
    return (1.0 - (lo / hi)) <= tolerance;
}

static void
normalize_minmax_u8(cv::Mat &img)
{
    double mn = 0.0;
    double mx = 0.0;
    cv::minMaxLoc(img, &mn, &mx, nullptr, nullptr);
    if (mx <= mn) {
        img.setTo(0);
        return;
    }

    const double scale = 255.0 / (mx - mn);
    const double shift = -mn * scale;
    img.convertTo(img, CV_8U, scale, shift);
}

/* CLAHE + minmax: melhora cristas em regiões de baixo contraste local,
 * aumentando keypoints SIFT detectados sem amplificar ruído global. */
static void
preprocess_for_sift(cv::Mat &img)
{
    auto clahe = cv::createCLAHE(2.0, cv::Size(8, 8));
    clahe->apply(img, img);
    normalize_minmax_u8(img);
}

static cv::Point2i
to_point_i(const cv::KeyPoint &kp)
{
    return cv::Point2i(cvRound(kp.pt.x), cvRound(kp.pt.y));
}

} // namespace

extern "C" int
cb2000_sigfm_opencv_pair_match(const uint8_t              *probe,
                               const uint8_t              *gallery,
                               int                         width,
                               int                         height,
                               const Cb2000SigfmCvConfig  *cfg,
                               Cb2000SigfmCvTelemetry     *tel)
{
    if (!probe || !gallery || !cfg || !tel || width <= 8 || height <= 8)
        return 0;

    *tel = {};

    cv::Mat probe_img(height, width, CV_8UC1, const_cast<uint8_t *>(probe));
    cv::Mat gallery_img(height, width, CV_8UC1, const_cast<uint8_t *>(gallery));
    probe_img = probe_img.clone();
    gallery_img = gallery_img.clone();

    preprocess_for_sift(probe_img);
    preprocess_for_sift(gallery_img);

    const auto sift = cv::SIFT::create();
    std::vector<cv::KeyPoint> probe_kp;
    std::vector<cv::KeyPoint> gallery_kp;
    cv::Mat probe_desc;
    cv::Mat gallery_desc;

    sift->detectAndCompute(probe_img, cv::noArray(), probe_kp, probe_desc);
    sift->detectAndCompute(gallery_img, cv::noArray(), gallery_kp, gallery_desc);

    tel->probe_keypoints = static_cast<unsigned int>(probe_kp.size());
    tel->gallery_keypoints = static_cast<unsigned int>(gallery_kp.size());

    if (probe_kp.size() < cfg->min_matches || gallery_kp.size() < cfg->min_matches) {
        tel->score = 0.0;
        return 1;
    }

    cv::BFMatcher matcher(cv::NORM_L2);
    std::vector<std::vector<cv::DMatch>> knn;
    matcher.knnMatch(probe_desc, gallery_desc, knn, 2);

    std::vector<CorrMatch> raw_matches;
    raw_matches.reserve(knn.size());
    for (const auto &pair : knn) {
        if (pair.size() < 2)
            continue;
        const cv::DMatch &m0 = pair[0];
        const cv::DMatch &m1 = pair[1];
        if (m0.distance < cfg->ratio_test * m1.distance) {
            raw_matches.push_back({to_point_i(probe_kp[m0.queryIdx]),
                                   to_point_i(gallery_kp[m0.trainIdx])});
        }
    }

    tel->raw_matches = static_cast<unsigned int>(raw_matches.size());
    if (raw_matches.size() < cfg->min_matches) {
        tel->score = 0.0;
        return 1;
    }

    std::set<CorrMatch> unique_set(raw_matches.begin(), raw_matches.end());
    std::vector<CorrMatch> uniq(unique_set.begin(), unique_set.end());
    tel->unique_matches = static_cast<unsigned int>(uniq.size());
    if (uniq.size() < cfg->min_matches) {
        tel->score = 0.0;
        return 1;
    }

    double dx_sum = 0.0;
    double dy_sum = 0.0;
    for (const auto &m : uniq) {
        dx_sum += static_cast<double>(m.p2.x - m.p1.x);
        dy_sum += static_cast<double>(m.p2.y - m.p1.y);
    }
    tel->shift_dx = dx_sum / static_cast<double>(uniq.size());
    tel->shift_dy = dy_sum / static_cast<double>(uniq.size());

    std::vector<AngleRec> angles;
    angles.reserve((uniq.size() * (uniq.size() - 1)) / 2);
    for (size_t i = 0; i < uniq.size(); i++) {
        for (size_t j = i + 1; j < uniq.size(); j++) {
            const double v1x = static_cast<double>(uniq[i].p1.x - uniq[j].p1.x);
            const double v1y = static_cast<double>(uniq[i].p1.y - uniq[j].p1.y);
            const double v2x = static_cast<double>(uniq[i].p2.x - uniq[j].p2.x);
            const double v2y = static_cast<double>(uniq[i].p2.y - uniq[j].p2.y);
            const double len1 = std::sqrt(v1x * v1x + v1y * v1y);
            const double len2 = std::sqrt(v2x * v2x + v2y * v2y);

            if (len1 <= 1e-9 || len2 <= 1e-9)
                continue;

            const double ratio_delta = 1.0 - std::min(len1, len2) / std::max(len1, len2);
            if (ratio_delta > cfg->length_match)
                continue;

            const double product = len1 * len2;
            const double dot_norm = clampd(((v1x * v2x) + (v1y * v2y)) / product, -1.0, 1.0);
            const double cross_norm = clampd(((v1x * v2y) - (v1y * v2x)) / product, -1.0, 1.0);

            angles.push_back({std::acos(cross_norm), (CV_PI / 2.0) + std::asin(dot_norm)});
        }
    }

    tel->angle_pairs = static_cast<unsigned int>(angles.size());
    if (angles.size() < cfg->min_matches) {
        tel->score = 0.0;
        return 1;
    }

    unsigned int votes = 0;
    bool matched = false;
    for (size_t i = 0; i < angles.size(); i++) {
        for (size_t j = i + 1; j < angles.size(); j++) {
            if (ratio_close(angles[i].sin_v, angles[j].sin_v, cfg->angle_match) &&
                ratio_close(angles[i].cos_v, angles[j].cos_v, cfg->angle_match)) {
                votes++;
                if (votes >= cfg->min_matches)
                    matched = true;
            }
        }
    }

    tel->consensus_pairs = votes;
    tel->original_match = matched ? 1u : 0u;

    const size_t max_vote_pairs = (angles.size() >= 2) ? ((angles.size() * (angles.size() - 1)) / 2) : 0;
    if (max_vote_pairs > 0)
        tel->inlier_ratio = clampd(static_cast<double>(votes) / static_cast<double>(max_vote_pairs), 0.0, 1.0);
    else
        tel->inlier_ratio = 0.0;

    if (matched)
        tel->score = 1.0;
    else
        tel->score = tel->inlier_ratio;

    return 1;
}

namespace {

static constexpr float  kMosaicRansacThresh   = 3.0f;
static constexpr int    kMosaicMinAffineInliers = 4;
static constexpr float  kMosaicDedupDistPx    = 3.0f;

struct MasterKp {
    float x, y, angle, response;
    float desc[CB2000_MOSAIC_DESC_LEN];
};

static bool
mosaic_is_dup(const std::vector<MasterKp> &master, float tx, float ty)
{
    for (const auto &m : master) {
        float dx = m.x - tx;
        float dy = m.y - ty;
        if (std::sqrt(dx * dx + dy * dy) < kMosaicDedupDistPx)
            return true;
    }
    return false;
}

/* R2.4 two-hop: pre-computed SIFT data for one gallery image */
struct GalleryData {
    std::vector<cv::KeyPoint> kp;
    cv::Mat                   desc;
};

/* Convert 2×3 affine to 3×3 homogeneous (bottom row = [0 0 1]) */
static cv::Mat
affine_to_h3(const cv::Mat &H)
{
    cv::Mat H3 = cv::Mat::eye(3, 3, CV_64F);
    H.copyTo(H3.rowRange(0, 2));
    return H3;
}

/* Try to find an affine transform src→dst via SIFT BFMatcher + RANSAC.
 * Returns 2×3 matrix and sets *n_inliers; returns empty Mat on failure. */
static cv::Mat
find_affine(const GalleryData         &src,
            const GalleryData         &dst,
            const Cb2000SigfmCvConfig *cfg,
            cv::BFMatcher             &matcher,
            int                       *n_inliers)
{
    *n_inliers = 0;
    if (src.kp.empty() || dst.kp.empty() || src.desc.empty() || dst.desc.empty())
        return cv::Mat();
    if (static_cast<int>(src.kp.size()) < kMosaicMinAffineInliers)
        return cv::Mat();

    std::vector<std::vector<cv::DMatch>> knn;
    matcher.knnMatch(src.desc, dst.desc, knn, 2);

    std::vector<cv::Point2f> src_pts, dst_pts;
    for (const auto &pair : knn) {
        if (pair.size() < 2) continue;
        const cv::DMatch &m0 = pair[0];
        const cv::DMatch &m1 = pair[1];
        if (m0.distance < static_cast<float>(cfg->ratio_test) * m1.distance) {
            src_pts.push_back(src.kp[m0.queryIdx].pt);
            dst_pts.push_back(dst.kp[m0.trainIdx].pt);
        }
    }

    if (static_cast<int>(src_pts.size()) < kMosaicMinAffineInliers)
        return cv::Mat();

    cv::Mat inlier_mask;
    cv::Mat H = cv::estimateAffine2D(src_pts, dst_pts, inlier_mask,
                                      cv::RANSAC,
                                      static_cast<double>(kMosaicRansacThresh));
    if (H.empty() || inlier_mask.empty())
        return cv::Mat();

    int n = 0;
    for (int r = 0; r < inlier_mask.rows; r++)
        if (inlier_mask.at<uchar>(r)) n++;

    if (n < kMosaicMinAffineInliers)
        return cv::Mat();

    *n_inliers = n;
    return H;
}

/* Add keypoints from one gallery into master, applying 2×3 affine H (or raw
 * if H is empty). Deduplication at kMosaicDedupDistPx. */
static void
add_keypoints(std::vector<MasterKp>   &master,
              const GalleryData        &g,
              const cv::Mat            &H,   /* 2×3 or empty */
              int                       max_kp)
{
    float rot_angle = 0.0f;
    bool  has_H     = !H.empty();

    if (has_H)
        rot_angle = static_cast<float>(
            std::atan2(H.at<double>(1, 0), H.at<double>(0, 0)) * 180.0 / CV_PI);

    for (int ki = 0; ki < static_cast<int>(g.kp.size()) &&
                     static_cast<int>(master.size()) < max_kp; ki++) {
        if (ki >= g.desc.rows) break;

        float tx = g.kp[ki].pt.x;
        float ty = g.kp[ki].pt.y;
        float tangle = g.kp[ki].angle;

        if (has_H) {
            tx = static_cast<float>(H.at<double>(0, 0) * g.kp[ki].pt.x +
                                    H.at<double>(0, 1) * g.kp[ki].pt.y +
                                    H.at<double>(0, 2));
            ty = static_cast<float>(H.at<double>(1, 0) * g.kp[ki].pt.x +
                                    H.at<double>(1, 1) * g.kp[ki].pt.y +
                                    H.at<double>(1, 2));
            tangle += rot_angle;
            if (tangle >= 360.0f) tangle -= 360.0f;
            if (tangle <   0.0f) tangle += 360.0f;
        }

        if (mosaic_is_dup(master, tx, ty)) continue;

        MasterKp m;
        m.x = tx; m.y = ty; m.angle = tangle;
        m.response = g.kp[ki].response;
        const float *row = g.desc.ptr<float>(ki);
        std::copy(row, row + CB2000_MOSAIC_DESC_LEN, m.desc);
        master.push_back(m);
    }
}

} // namespace

extern "C" int
cb2000_sigfm_opencv_build_mosaic(const uint8_t * const     *images,
                                 int                        n_images,
                                 int                        width,
                                 int                        height,
                                 const Cb2000SigfmCvConfig *cfg,
                                 Cb2000MosaicKeypoint      *out_kp,
                                 int                        max_kp)
{
    if (!images || n_images < 1 || !cfg || !out_kp || max_kp < 1)
        return 0;
    if (width <= 8 || height <= 8)
        return 0;

    /* --- Step 1: pre-compute SIFT for all galleries --- */
    const auto sift = cv::SIFT::create();
    std::vector<GalleryData> gdata(static_cast<size_t>(n_images));
    for (int i = 0; i < n_images; i++) {
        cv::Mat img(height, width, CV_8UC1,
                    const_cast<uint8_t *>(images[i]));
        img = img.clone();
        preprocess_for_sift(img);
        sift->detectAndCompute(img, cv::noArray(), gdata[i].kp, gdata[i].desc);
    }

    cv::BFMatcher matcher(cv::NORM_L2);

    /* H3_map[i] = 3×3 homogeneous transform: gallery[i] → gallery[0] frame.
     * Empty = not yet aligned. gallery[0] gets identity. */
    std::vector<cv::Mat> H3_map(static_cast<size_t>(n_images));
    /* aligned_direct[i] = successfully aligned to gallery[0] in pass 0.
     * Used as eligible bridges in pass 1 (caps composed depth at 2 hops). */
    std::vector<bool> aligned_direct(static_cast<size_t>(n_images), false);

    H3_map[0]        = cv::Mat::eye(3, 3, CV_64F);
    aligned_direct[0] = true;

    std::vector<MasterKp> master;
    master.reserve(static_cast<size_t>(max_kp));

    /* Seed master with gallery[0] keypoints (reference frame) */
    add_keypoints(master, gdata[0], cv::Mat(), max_kp);

    /* --- Pass 0: direct alignment to gallery[0] --- */
    int n_direct = 0, n_twohop = 0, n_raw = 0;
    for (int idx = 1; idx < n_images; idx++) {
        if (gdata[idx].kp.empty() || gdata[idx].desc.empty()) {
            n_raw++;
            continue;
        }
        int n_inliers = 0;
        cv::Mat H = find_affine(gdata[idx], gdata[0], cfg, matcher, &n_inliers);
        if (!H.empty()) {
            H3_map[idx]        = affine_to_h3(H);
            aligned_direct[idx] = true;
            add_keypoints(master, gdata[idx], H, max_kp);
            n_direct++;
        }
    }

    /* --- Pass 1: two-hop — align unaligned galleries via a pass-0 bridge ---
     * For each unaligned gallery[idx], find the pass-0-aligned gallery[j]
     * that yields the most inliers, then compose:
     *   H_ref_from_idx = H3_map[j] * affine_to_h3(H_idx_to_j)
     * This bounds composed depth to exactly 2 hops (gallery[0]←j←idx). */
    for (int idx = 1; idx < n_images; idx++) {
        if (aligned_direct[idx]) continue;
        if (gdata[idx].kp.empty() || gdata[idx].desc.empty()) {
            n_raw++;
            continue;
        }

        cv::Mat best_H;
        int     best_inliers = 0;

        for (int j = 1; j < n_images; j++) {
            if (!aligned_direct[j] || j == idx) continue;
            int n_inliers = 0;
            cv::Mat H_ij = find_affine(gdata[idx], gdata[j],
                                        cfg, matcher, &n_inliers);
            if (!H_ij.empty() && n_inliers > best_inliers) {
                best_inliers = n_inliers;
                cv::Mat composed = H3_map[j] * affine_to_h3(H_ij);
                best_H = composed.rowRange(0, 2).clone();
            }
        }

        if (!best_H.empty()) {
            H3_map[idx] = affine_to_h3(best_H);
            add_keypoints(master, gdata[idx], best_H, max_kp);
            n_twohop++;
        } else {
            /* Still unaligned: add raw (original coordinates, known noise) */
            add_keypoints(master, gdata[idx], cv::Mat(), max_kp);
            n_raw++;
        }
    }

    fprintf(stderr,
            "[ MOSAIC_BUILD ] galleries=%d direct=%d twohop=%d raw=%d "
            "master_kp=%d\n",
            n_images, n_direct, n_twohop, n_raw,
            static_cast<int>(master.size()));

    /* --- Write output --- */
    int n_out = std::min(static_cast<int>(master.size()), max_kp);
    for (int i = 0; i < n_out; i++) {
        out_kp[i].x     = master[i].x;
        out_kp[i].y     = master[i].y;
        out_kp[i].angle = master[i].angle;
        std::copy(master[i].desc,
                  master[i].desc + CB2000_MOSAIC_DESC_LEN,
                  out_kp[i].desc);
    }
    return n_out;
}

extern "C" int
cb2000_sigfm_opencv_match_mosaic(const uint8_t             *probe,
                                 int                        width,
                                 int                        height,
                                 const Cb2000MosaicKeypoint *mosaic_kp,
                                 int                        mosaic_count,
                                 const Cb2000SigfmCvConfig *cfg,
                                 Cb2000SigfmCvTelemetry    *tel)
{
    if (!probe || !mosaic_kp || !cfg || !tel || mosaic_count < 1)
        return 0;
    if (width <= 8 || height <= 8)
        return 0;

    *tel = {};

    cv::Mat probe_img(height, width, CV_8UC1,
                      const_cast<uint8_t *>(probe));
    probe_img = probe_img.clone();
    preprocess_for_sift(probe_img);

    const auto sift = cv::SIFT::create();
    std::vector<cv::KeyPoint> probe_kp;
    cv::Mat probe_desc;
    sift->detectAndCompute(probe_img, cv::noArray(), probe_kp, probe_desc);

    tel->probe_keypoints  = static_cast<unsigned int>(probe_kp.size());
    tel->gallery_keypoints = static_cast<unsigned int>(mosaic_count);

    if (static_cast<int>(probe_kp.size()) < static_cast<int>(cfg->min_matches) ||
        probe_desc.empty()) {
        tel->score = 0.0;
        return 1;
    }

    /* Build descriptor matrix from mosaic keypoints */
    cv::Mat mosaic_desc(mosaic_count, CB2000_MOSAIC_DESC_LEN, CV_32F);
    for (int i = 0; i < mosaic_count; i++) {
        float *row = mosaic_desc.ptr<float>(i);
        for (int j = 0; j < CB2000_MOSAIC_DESC_LEN; j++)
            row[j] = mosaic_kp[i].desc[j];
    }

    cv::BFMatcher matcher(cv::NORM_L2);
    std::vector<std::vector<cv::DMatch>> knn;
    matcher.knnMatch(probe_desc, mosaic_desc, knn, 2);

    std::vector<cv::Point2f> probe_pts, mosaic_pts;
    for (const auto &pair : knn) {
        if (pair.size() < 2)
            continue;
        const cv::DMatch &m0 = pair[0];
        const cv::DMatch &m1 = pair[1];
        if (m0.distance < static_cast<float>(cfg->ratio_test) * m1.distance) {
            probe_pts.push_back(probe_kp[m0.queryIdx].pt);
            mosaic_pts.push_back(
                cv::Point2f(mosaic_kp[m0.trainIdx].x,
                            mosaic_kp[m0.trainIdx].y));
        }
    }

    tel->raw_matches    = static_cast<unsigned int>(probe_pts.size());
    tel->unique_matches = tel->raw_matches;
    tel->angle_pairs    = tel->raw_matches; /* total ratio-test matches */

    if (static_cast<int>(probe_pts.size()) < static_cast<int>(cfg->min_matches)) {
        tel->score = 0.0;
        return 1;
    }

    cv::Mat H;
    cv::Mat inlier_mask;
    H = cv::estimateAffine2D(probe_pts, mosaic_pts, inlier_mask,
                              cv::RANSAC,
                              static_cast<double>(kMosaicRansacThresh));

    unsigned int inliers = 0;
    if (!H.empty() && !inlier_mask.empty()) {
        for (int r = 0; r < inlier_mask.rows; r++)
            if (inlier_mask.at<uchar>(r))
                inliers++;
        tel->shift_dx = H.at<double>(0, 2);
        tel->shift_dy = H.at<double>(1, 2);
    }

    tel->consensus_pairs = inliers;
    const double total = static_cast<double>(probe_pts.size());
    tel->inlier_ratio   = (total > 0.0) ?
                          clampd(static_cast<double>(inliers) / total, 0.0, 1.0) :
                          0.0;

    const bool matched = (inliers >= cfg->min_matches);
    tel->original_match = matched ? 1u : 0u;
    tel->score = matched ? 1.0 : tel->inlier_ratio;

    return 1;
}
