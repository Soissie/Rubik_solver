/* Rubiks cube scanner (HAN EVD1, period 1 final assignment)
    Reads the 9 sticker colors of each face of a 3x3 cube through a webcam and saves all 6 faces.
    Hold one face inside the guide grid in the middle of the view and keep it still, the face is saved automatically.
    Keys: q = quit, r = remove last saved face, p = print the current colors to terminal.
    
    steps (explained in README.md):
    1. acquisition: live webcam frames
    2. enhancement: CLAHE on L channel
    3. segmentation: contours to sticker squares, one per grid cell.
    4. feature extraction: blur, canny, dilate and the lab color per sticker
    5. classification: blob matching over frames
    */

#include <opencv2/opencv.hpp>
#include <array>
#include <numeric>
#include <iostream>
#include <cmath>

//  Step 1: Display 
constexpr double grid_scale = 0.2; // Scale of the grid, compared to min(width/height)
constexpr int cube_order = 3; // Order = 3x3
constexpr int line_thickness = 1; // Grid thickness
constexpr int text_thickness = 2; 

// Step 2: enhancement (tuned 31 Oct, mainly on blue and white faces)
constexpr double clahe_clip_limit = 2.0; // Cap on contrast boost
constexpr int clahe_tile_size = 8; // The amount of tiles (8x8)

// Step 3/4: edges + squares 
const cv::Size gaussian_size(3, 3); // must be odd, 3 works best in this case
constexpr int sigma = 0; // 0 = openCV picks bsaed on kernel size
constexpr int canny_low = 20;  // Canny ratio is 1/3
constexpr int canny_high = 60;
constexpr double arc_margin = 0.1; // Square tolerance
constexpr double min_square_area_ratio = 0.15; // Smallest sticker as a part of one cell area, scales with the grid
constexpr double min_aspect_ratio = 0.8;
constexpr double max_aspect_ratio = 1.2;

// Step 5: classification 
constexpr int min_found_stickers = 8;          // At most 1 inferred sticker (the logo on the white face requires this)
constexpr int stable_frames_needed = 10;       // Still frames needed before a face is saved
constexpr double max_blob_move = 0.2;          // Max movement per frame, in cells
constexpr double same_center_distance = 25.0;  // Max Lab distance for 2 centers to count as the same color
constexpr int faces_needed = 6;

// Creates guide 3x3
void draw_guide_grid(cv::Mat &img, const cv::Rect &grid_rect)
{
    cv::rectangle(img, grid_rect, cv::Scalar(0, 255, 0));
    for (int i = 1; i < cube_order; i++) { // i = 1 or 2 for the 2 horizontal and vertical lines to separate grids.
        cv::Point v_start(grid_rect.x + grid_rect.width * i / cube_order, grid_rect.y);
        cv::Point v_end(grid_rect.x + grid_rect.width * i / cube_order, grid_rect.y + grid_rect.height);
        cv::Point h_start(grid_rect.x, grid_rect.y + grid_rect.height * i / cube_order);
        cv::Point h_end(grid_rect.x + grid_rect.width, grid_rect.y + grid_rect.height * i / cube_order);
        cv::line(img, v_start, v_end, cv::Scalar(255, 0, 0), line_thickness);
        cv::line(img, h_start, h_end, cv::Scalar(255, 0, 0), line_thickness);
    }
}

// Step 2: enhancement 
// CLAHE on L (lightness) only: sharper sticker rims for finding squares, colors are read from lab_frame later.
cv::Mat enhance(const cv::Mat &lab_frame, const cv::Ptr<cv::CLAHE> &clahe)
{
    std::vector<cv::Mat> channels;
    cv::split(lab_frame, channels);
    cv::Mat l_enhanced;
    clahe->apply(channels[0], l_enhanced);
    return l_enhanced;
}

// Step 3/4: Blur, canny and dilate to find edges.
cv::Mat find_edges(const cv::Mat &l_enhanced)
{
    cv::Mat blurred;
    cv::Mat edges;
    cv::GaussianBlur(l_enhanced, blurred, gaussian_size, sigma);
    cv::Canny(blurred, edges, canny_low, canny_high);
    cv::dilate(edges, edges, cv::Mat()); // Closes small gaps in the outlines
    return edges;
}

//Step 3: find squares 
// Out: all_squares
std::vector<std::vector<cv::Point>> find_squares(const cv::Mat &edges, const cv::Rect &grid_rect)
{
    int cell = grid_rect.width / cube_order;
    double min_square_area = min_square_area_ratio * cell * cell; // min square area based on the size of the grid
    std::vector<std::vector<cv::Point>> contours;
    std::vector<cv::Vec4i> hierarchy;
    std::vector<cv::Point> approx;
    std::vector<std::vector<cv::Point>> all_squares;

    // Each contour gets a parent if its inside an outline
    cv::findContours(edges, contours, hierarchy, cv::RETR_CCOMP, cv::CHAIN_APPROX_SIMPLE);
    for (size_t i = 0; i < contours.size(); i++) {
        // no parent = outside of an outline: keep only the inside, so each sticker counts once
        if (hierarchy[i][3] == -1) {
            continue;
        }
        // Sijmplifies a wobbly outline to the fewest corners.
        cv::approxPolyDP(contours[i], approx, arc_margin * cv::arcLength(contours[i], true), true);
        cv::Rect box = cv::boundingRect(approx);
        // Skips squares outside the grid (background)
        if (!grid_rect.contains((box.tl() + box.br()) / 2)) {
            continue;
        }
        double aspect_ratio = static_cast<double>(box.width) / box.height;
        // Checks for 4 corners, convexity, size and squareness.
        if (approx.size() == 4 && cv::isContourConvex(approx) &&
            cv::contourArea(approx) >= min_square_area &&
            aspect_ratio > min_aspect_ratio && aspect_ratio < max_aspect_ratio) {
            all_squares.push_back(approx);
        }
    }
    return all_squares;
}

// Out: centers, known (detected or inferred). Returns how many were detected
int find_sticker_centers(const std::vector<std::vector<cv::Point>> &all_squares, const cv::Rect &grid_rect,
                         std::array<cv::Point2f, 9> &centers, std::array<bool, 9> &known)
{
    int cell = grid_rect.width / cube_order;
    int found = 0;
    std::array<bool, 9> detected{}; // Starts all false
    known.fill(false);
    // square_center = average of the 4 corners.
    for (const auto &square : all_squares) {
        cv::Point sum = std::accumulate(square.begin(), square.end(), cv::Point(0, 0));
        cv::Point2f square_center = cv::Point2f(sum) / 4.0f;
        // Square in the grid = center of its cell, counted once per cell
        if(grid_rect.contains(square_center)){
            // Gives each cell number 0-8
            int k = static_cast<int>((square_center.x - grid_rect.x)/cell)
            + static_cast<int>((square_center.y - grid_rect.y)/cell) * 3;
            centers[k] = square_center;
            if (!detected[k]) {
                found++;
            }
            detected[k] = true;
            known[k] = true;
        }
    }

    // Horizontal step between neighbours, averaged over every detected pair.
    // Vertical pairs count too, turned by 90 degrees since both steps are the same on a square grid
    cv::Point2f step_sum(0, 0);
    int pairs = 0;
    int anchor = -1; // A detected sticker, the missing ones are calculated from it
    for (int k = 0; k < 9; k++) {
        // Checks for direct neighbors
        if (detected[k]) {
            anchor = k;
        }
        if (k % 3 < 2 && detected[k] && detected[k + 1]) {
            step_sum += centers[k + 1] - centers[k];
            pairs++;
        }
        if (k < 6 && detected[k] && detected[k + 3]) {
            cv::Point2f d = centers[k + 3] - centers[k];
            step_sum += cv::Point2f(d.y, -d.x);
            pairs++;
        }
    }

    // Missing sticker = anchor + col diff * h_step + row diff * v_step
    // No detected pairs means nothing to infer from, they stay unknown
    if (pairs == 0) {
        return found;
    }
    cv::Point2f h_step = step_sum / static_cast<float>(pairs);
    cv::Point2f v_step(-h_step.y, h_step.x);
    
    for (int k = 0; k < 9; k++) {
        if (detected[k]) {
            continue;
        }
        int col_diff = k % 3 - anchor % 3;
        int row_diff = k / 3 - anchor / 3;
        centers[k] = centers[anchor] + h_step * col_diff + v_step * row_diff;
        known[k] = true;
    }
    return found;
}

// Step 4: read colors
// Color of one sticker: 4 small patches around the center, the 2 most similar are averaged.
// Ignores the logo, which runs diagonally through 2 of them
// Returns false if the sticker is partly off screen
bool read_color(const cv::Mat &lab, cv::Point2f center, int cell, cv::Vec3f &color)
{
    float offset = 0.2f * cell;
    int half = std::max(1, static_cast<int>(0.08 * cell));
    const cv::Point2f directions[4] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    std::array<cv::Vec3f, 4> patch;
    for (int p = 0; p < 4; p++) {
        cv::Point patch_center = center + directions[p] * offset;
        cv::Rect roi(patch_center.x - half, patch_center.y - half, 2 * half + 1, 2 * half + 1);
        if ((roi & cv::Rect(0, 0, lab.cols, lab.rows)) != roi) {
            return false;
        }
        cv::Scalar mean = cv::mean(lab(roi));
        patch[p] = cv::Vec3f(mean[0], mean[1], mean[2]);
    }

    // Finds the 2 patches with the smallest color difference
    int best_a = 0;
    int best_b = 1;
    for (int a = 0; a < 4; a++) {
        for (int b = a + 1; b < 4; b++) {
            if (cv::norm(patch[a] - patch[b]) < cv::norm(patch[best_a] - patch[best_b])) {
                best_a = a;
                best_b = b;
            }
        }
    }
    color = (patch[best_a] + patch[best_b]) * 0.5f;
    return true;
}

// Step 5: classification
// Returns the face whose center color is closest
int nearest_center(const cv::Vec3f &color, const std::vector<std::array<cv::Vec3f, 9>> &faces)
{
    int best = 0;
    for (size_t f = 1; f < faces.size(); f++) {
        if (cv::norm(color - faces[f][4]) < cv::norm(color - faces[best][4])) {
            best = f;
        }
    }
    return best;
}

// Lab to BGR for drawing
cv::Scalar lab_to_bgr(const cv::Vec3f &lab)
{
    cv::Mat pixel(1, 1, CV_8UC3, cv::Scalar(lab[0], lab[1], lab[2]));
    cv::cvtColor(pixel, pixel, cv::COLOR_Lab2BGR);
    cv::Vec3b bgr = pixel.at<cv::Vec3b>(0, 0);
    return cv::Scalar(bgr[0], bgr[1], bgr[2]);
}

// Draws the saved faces as 3x3 blocks at the top.
// Measured colors until all 6 are saved, then the color of the matching center
void draw_faces(cv::Mat &img, const std::vector<std::array<cv::Vec3f, 9>> &faces)
{
    constexpr int tile = 20;
    for (size_t f = 0; f < faces.size(); f++) {
        for (int k = 0; k < 9; k++) {
            cv::Vec3f shown = faces[f][k];
            if (faces.size() == faces_needed) {
                shown = faces[nearest_center(faces[f][k], faces)][4];
            }
            cv::Rect r(10 + f * (3 * tile + 10) + (k % 3) * tile, 50 + (k / 3) * tile, tile, tile);
            cv::rectangle(img, r, lab_to_bgr(shown), cv::FILLED);
            cv::rectangle(img, r, cv::Scalar(0, 0, 0), 1);
        }
    }
}


int main()
{
    cv::Mat frame;
    cv::Mat lab_frame;

    cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(clahe_clip_limit, cv::Size(clahe_tile_size, clahe_tile_size));

    std::vector<std::array<cv::Vec3f, 9>> faces; // Saved faces, 9 Lab colors each
    std::array<cv::Point2f, 9> previous_centers{};
    int stable_frames = 0;

    // Step 1: acquisition
    cv::VideoCapture cap(0);
    if (!cap.isOpened()) {
        std::cerr << "Error, could not open webcam\n";
        return -1;
    }

    while (true) {
        cap >> frame;
        if (frame.empty()) {
            std::cerr << "Error, the input frame is empty.\n";
            break;
        }

        // Guide grid in the middle, used for drawing and for the cells
        int grid_size = grid_scale * std::min(frame.cols, frame.rows);
        cv::Rect grid_rect((frame.cols - grid_size) / 2, (frame.rows - grid_size) / 2, grid_size, grid_size);
        cv::Mat display = frame.clone();
        draw_guide_grid(display, grid_rect);

        cv::cvtColor(frame, lab_frame, cv::COLOR_BGR2Lab);
        cv::Mat l_enhanced = enhance(lab_frame, clahe);
        cv::Mat edges = find_edges(l_enhanced);
        std::vector<std::vector<cv::Point>> all_squares = find_squares(edges, grid_rect);

        std::array<cv::Point2f, 9> centers{};
        std::array<bool, 9> known;
        int found = find_sticker_centers(all_squares, grid_rect, centers, known);
        // Circle on every detected or inferred center
        for (int k = 0; k < 9; k++) {
            if (known[k]) {
                cv::circle(display, centers[k], 5, cv::Scalar(0, 255, 255), 2);
            }
        }

        // Step 4: read colors
        std::array<cv::Vec3f, 9> colors;
        std::array<bool, 9> color_ok{};
        for (int k = 0; k < 9; k++) {
            if (known[k]) {
                color_ok[k] = read_color(lab_frame, centers[k], grid_size / cube_order, colors[k]);
            }
        }

        // Step 5: blob matching across frames
        // Same 9 blobs in the same place for 10 frames = the face is held still
        int cell = grid_size / cube_order;
        bool same_blobs = found >= min_found_stickers;
        for (int k = 0; k < 9; k++) {
            if (!color_ok[k] || cv::norm(centers[k] - previous_centers[k]) > max_blob_move * cell) {
                same_blobs = false;
            }
        }
        previous_centers = centers;
        stable_frames = same_blobs ? stable_frames + 1 : 0;

        if (stable_frames == stable_frames_needed) {
            // Center matches a saved one = rescan, otherwise a new face
            int match = -1;
            for (size_t f = 0; f < faces.size(); f++) {
                if (cv::norm(colors[4] - faces[f][4]) < same_center_distance) {
                    match = f;
                }
            }
            if (match >= 0) {
                faces[match] = colors;
                std::cout << "Rescanned face " << match << "\n";
            } else if (faces.size() < faces_needed) {
                faces.push_back(colors);
                std::cout << "Saved face " << faces.size() - 1 << ", center " << colors[4] << "\n";
            }

            // Step 5: classification
            // With all 6 saved, each sticker gets the number of the closest center
            if (faces.size() == faces_needed) {
                for (size_t f = 0; f < faces.size(); f++) {
                    std::cout << "Face " << f << ":";
                    for (int k = 0; k < 9; k++) {
                        std::cout << " " << nearest_center(faces[f][k], faces);
                    }
                    std::cout << "\n";
                }
                std::cout << "\n";
            }
        }

        // Output
        draw_faces(display, faces);
        std::string count_text = "faces: " + std::to_string(faces.size()) + "/6  found: " + std::to_string(found);
        cv::polylines(display, all_squares, true, cv::Scalar(255, 255, 0), line_thickness);
        cv::putText(display, count_text, cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 255, 0), text_thickness);
        cv::imshow("cube scanner", display);

        int key = cv::waitKey(1);
        // q quits, r removes the last saved face
        if (key == 'q') {
            break;
        }
        if (key == 'r' && !faces.empty()) {
            faces.pop_back();
            std::cout << "Removed the last face\n";
        }
        if (key == 'p') {
            for (int k = 0; k < 9; k++) {
                if (color_ok[k]) {
                    std::cout << k << ": " << colors[k] << "\n";
                } else {
                    std::cout << k << ": unknown\n";
                }
            }
            std::cout << "\n";
        }
    }
    return 0;
}
