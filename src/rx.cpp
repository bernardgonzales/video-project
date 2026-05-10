/**
 * rx.cpp - GStreamer RTP Receiver with YOLOv8 Object Detection
 *
 * Receives an RTP H264 stream over UDP, decodes it, runs YOLOv8 inference
 * via OpenCV DNN, and displays annotated frames.
 *
 * Pipeline:
 *   udpsrc → queue → rtph264depay → h264parse → avdec_h264
 *         → videoconvert → capsfilter(BGR) → appsink
 *
 * Build:
 *   make bin/rx
 *
 * Run:
 *   ./bin/rx [yolov4-tiny.cfg] [yolov4-tiny.weights] [coco.names]
 *
 * Required files (download separately):
 *   yolov4-tiny.cfg     — https://github.com/AlexeyAB/darknet/raw/master/cfg/yolov4-tiny.cfg
 *   yolov4-tiny.weights — https://github.com/AlexeyAB/darknet/releases/download/darknet_yolo_v4_pre/yolov4-tiny.weights
 *   coco.names          — one class label per line (80 lines for COCO)
 */

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#define RX_PORT 5000
#define RTP_PT  96

#define RTP_CAPS \
    "application/x-rtp,"   \
    "media=video,"          \
    "encoding-name=H264,"   \
    "payload=" G_STRINGIFY(RTP_PT)

// Lower CONF_THRESHOLD → more detections, more false positives (range 0–1)
// Higher CONF_THRESHOLD → fewer detections, fewer false positives
static const float CONF_THRESHOLD = 0.5f;

// Lower NMS_THRESHOLD → aggressively merge overlapping boxes (fewer duplicates)
// Higher NMS_THRESHOLD → keep more overlapping boxes (useful for dense crowds)
static const float NMS_THRESHOLD  = 0.45f;

// Must match the resolution the model was trained at.
// yolov4-tiny: 416   |   yolov4 (full): 608   |   yolov8n: 640
static const int   INPUT_SIZE     = 416;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::vector<std::string> load_class_names(const std::string &path)
{
    std::vector<std::string> names;
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[RX] WARNING: could not open " << path
                  << " — detections will have no labels\n";
        return names;
    }
    std::string line;
    while (std::getline(file, line))
        if (!line.empty()) names.push_back(line);
    return names;
}

// Run YOLOv4 darknet inference on `frame` (BGR) and draw results in-place.
static void run_yolo(cv::dnn::Net &net, const std::vector<std::string> &class_names, cv::Mat &frame)
{
    const int orig_w = frame.cols;
    const int orig_h = frame.rows;

    cv::Mat blob;
    cv::dnn::blobFromImage(frame, blob, 1.0 / 255.0,
                           cv::Size(INPUT_SIZE, INPUT_SIZE),
                           cv::Scalar(), true, false);
    net.setInput(blob);

    // YOLOv4 has multiple output layers; each row is [cx, cy, w, h, obj, cls...]
    std::vector<cv::Mat> raw_outputs;
    net.forward(raw_outputs, net.getUnconnectedOutLayersNames());

    const int n_cls = static_cast<int>(class_names.size());

    std::vector<cv::Rect> boxes;
    std::vector<float>    scores;
    std::vector<int>      class_ids;

    // Filter and draw
    for (const cv::Mat &out : raw_outputs) 
    {
        const auto *data = reinterpret_cast<const float *>(out.data);
        for (int i = 0; i < out.rows; i++, data += out.cols) {
            float obj_conf = data[4];
            if (obj_conf < CONF_THRESHOLD) continue;

            cv::Mat class_scores(1, n_cls, CV_32F,
                                 const_cast<float *>(data + 5));
            cv::Point best_class;
            double    best_score;
            cv::minMaxLoc(class_scores, nullptr, &best_score,
                          nullptr, &best_class);

            float conf = obj_conf * static_cast<float>(best_score);
            if (conf < CONF_THRESHOLD) continue;

            int cx = static_cast<int>(data[0] * orig_w);
            int cy = static_cast<int>(data[1] * orig_h);
            int w  = static_cast<int>(data[2] * orig_w);
            int h  = static_cast<int>(data[3] * orig_h);

            boxes.push_back(cv::Rect(cx - w / 2, cy - h / 2, w, h));
            scores.push_back(conf);
            class_ids.push_back(best_class.x);
        }
    }

    std::vector<int> keep;
    cv::dnn::NMSBoxes(boxes, scores, CONF_THRESHOLD, NMS_THRESHOLD, keep);

    for (int idx : keep) {
        const cv::Rect &box = boxes[idx];
        std::string label   = (class_ids[idx] < n_cls)
                              ? class_names[class_ids[idx]] : "unknown";
        label += " " + std::to_string(static_cast<int>(scores[idx] * 100)) + "%";

        cv::rectangle(frame, box, cv::Scalar(0, 255, 0), 2);
        cv::putText(frame, label,
                    cv::Point(box.x, std::max(box.y - 5, 0)),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5,
                    cv::Scalar(0, 255, 0), 1);
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    const char *cfg_path    = argc > 1 ? argv[1] : "yolov4-tiny.cfg";
    const char *weights_path= argc > 2 ? argv[2] : "yolov4-tiny.weights";
    const char *names_path  = argc > 3 ? argv[3] : "coco.names";

    // Load YOLOv4 darknet model
    std::cout << "[RX] Loading model: " << cfg_path << " + " << weights_path << "\n";
    cv::dnn::Net net = cv::dnn::readNetFromDarknet(cfg_path, weights_path);
    if (net.empty()) 
    {
        std::cerr << "[RX] ERROR: failed to load YOLO model.\n";
        return -1;
    }
    // To use GPU: DNN_BACKEND_CUDA / DNN_TARGET_CUDA
    net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
    net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);

    auto class_names = load_class_names(names_path);
    std::cout << "[RX] Loaded " << class_names.size() << " class names.\n";

    // GStreamer
    gst_init(&argc, &argv);
    std::cout << "[RX] Initializing GStreamer...\n";

    GstElement *pipeline   = gst_pipeline_new("rx-pipeline");
    GstElement *source     = gst_element_factory_make("udpsrc",       "source");
    GstElement *queue      = gst_element_factory_make("queue",        "queue");
    GstElement *depay      = gst_element_factory_make("rtph264depay", "depay");
    GstElement *parser     = gst_element_factory_make("h264parse",    "parser");
    GstElement *decoder    = gst_element_factory_make("avdec_h264",   "decoder");
    GstElement *convert    = gst_element_factory_make("videoconvert", "convert");
    GstElement *capsfilter = gst_element_factory_make("capsfilter",   "capsfilter");
    GstElement *appsink    = gst_element_factory_make("appsink",      "sink");

    if (!pipeline || !source || !queue || !depay || !parser ||
        !decoder  || !convert || !capsfilter || !appsink) 
    {
        std::cerr << "[RX] ERROR: failed to create pipeline elements.\n";
        return -1;
    }

    // udpsrc: listen port + RTP caps + large socket buffer to absorb high-res frame bursts
    GstCaps *rtp_caps = gst_caps_from_string(RTP_CAPS);
    g_object_set(source, "port", RX_PORT, "caps", rtp_caps,
                 "buffer-size", 4 * 1024 * 1024,   // 4 MB socket receive buffer
                 NULL);
    gst_caps_unref(rtp_caps);

    // queue: enough room for several large frames worth of RTP packets
    g_object_set(queue,
                 "max-size-buffers", 0,
                 "max-size-bytes",   8 * 1024 * 1024,   // 8 MB
                 "max-size-time",    0,
                 NULL);

    // Force BGR so we can wrap the buffer directly in cv::Mat
    GstCaps *bgr_caps = gst_caps_from_string("video/x-raw,format=BGR");
    g_object_set(capsfilter, "caps", bgr_caps, NULL);
    gst_caps_unref(bgr_caps);

    // appsink: pull mode, drop stale frames when inference is slow
    g_object_set(appsink,
                 "emit-signals", FALSE,
                 "sync",         FALSE,
                 "drop",         TRUE,
                 "max-buffers",  1,
                 NULL);

    gst_bin_add_many(GST_BIN(pipeline),
                     source, queue, depay, parser,
                     decoder, convert, capsfilter, appsink, NULL);

    if (!gst_element_link_many(source, queue, depay, parser,
                               decoder, convert, capsfilter, appsink, NULL)) 
    {
        std::cerr << "[RX] ERROR: failed to link pipeline.\n";
        gst_object_unref(pipeline);
        return -1;
    }

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) 
    {
        std::cerr << "[RX] ERROR: failed to set pipeline to PLAYING.\n";
        gst_object_unref(pipeline);
        return -1;
    }

    std::cout << "[RX] Listening on port " << RX_PORT << " — press 'q' in the display window to stop.\n";

    GstBus *bus = gst_element_get_bus(pipeline);

    while (true) 
    {
        // Check for pipeline-level errors or EOS
        GstMessage *msg = gst_bus_pop_filtered(
            bus,
            static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        if (msg) 
        {
            if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) 
            {
                GError *err = nullptr; gchar *dbg = nullptr;
                gst_message_parse_error(msg, &err, &dbg);
                std::cerr << "[RX] ERROR: " << err->message << "\n";
                if (dbg) 
                { 
                    std::cerr << "[RX] Debug: " << dbg << "\n"; g_free(dbg); 
                }
                g_error_free(err);
            } 
            else 
            {
                std::cout << "[RX] End of stream.\n";
            }
            gst_message_unref(msg);
            break;
        }

        // Pull the latest decoded frame (100 ms timeout)
        GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink), 100 * GST_MSECOND);
        if (!sample) continue;

        // Resolve frame dimensions from caps
        GstCaps      *caps = gst_sample_get_caps(sample);
        GstStructure *s    = gst_caps_get_structure(caps, 0);
        int width = 0, height = 0;
        gst_structure_get_int(s, "width",  &width);
        gst_structure_get_int(s, "height", &height);

        // Zero-copy wrap of the GStreamer buffer as a cv::Mat
        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstMapInfo map;
        gst_buffer_map(buffer, &map, GST_MAP_READ);

        cv::Mat frame(height, width, CV_8UC3, map.data);

        run_yolo(net, class_names, frame);
        cv::imshow("RX - YOLO Detection", frame);

        gst_buffer_unmap(buffer, &map);
        gst_sample_unref(sample);

        if (cv::waitKey(1) == 'q') break;
    }

    std::cout << "[RX] Shutting down...\n";
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    cv::destroyAllWindows();

    std::cout << "[RX] Done.\n";
    return 0;
}
