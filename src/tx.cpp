/**
 * tx.cpp - GStreamer RTP Transmitter
 *
 * Webcam: v4l2src → capsfilter(YUY2) → videoconvert → capsfilter(I420) → x264enc → rtph264pay → udpsink
 * File:   filesrc → qtdemux/tsdemux → h264parse → rtph264pay → udpsink
 *
 * Usage:
 *   ./tx                          webcam on /dev/video0
 *   ./tx /dev/videoN              webcam on /dev/videoN
 *   ./tx --file path/to/video     file mode (.mp4 or .ts)
 */

#include <gst/gst.h>
#include <iostream>
#include <string>

#define TX_HOST  "127.0.0.1"
#define TX_PORT  5000
#define RTP_PT   96

// Called when tsdemux/qtdemux exposes a src pad; links the first H264 video pad to h264parse.
static void on_pad_added(GstElement * /*src*/, GstPad *pad, gpointer data)
{
    GstElement *h264parse = static_cast<GstElement *>(data);
    GstPad *sink = gst_element_get_static_pad(h264parse, "sink");

    if (gst_pad_is_linked(sink)) {
        gst_object_unref(sink);
        return;
    }

    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, NULL);

    GstStructure *s = gst_caps_get_structure(caps, 0);
    if (g_str_has_prefix(gst_structure_get_name(s), "video/x-h264"))
        gst_pad_link(pad, sink);

    gst_caps_unref(caps);
    gst_object_unref(sink);
}

static GstElement *build_file_pipeline(const std::string &path)
{
    // Pick demuxer based on extension
    std::string demux_name;
    if (path.size() >= 3 && path.substr(path.size() - 3) == ".ts")
        demux_name = "tsdemux";
    else
        demux_name = "qtdemux"; // covers .mp4, .mov, .m4v

    GstElement *pipeline  = gst_pipeline_new("tx-pipeline");
    GstElement *source    = gst_element_factory_make("filesrc",       "source");
    GstElement *demux     = gst_element_factory_make(demux_name.c_str(), "demux");
    GstElement *parse     = gst_element_factory_make("h264parse",     "parse");
    GstElement *pay       = gst_element_factory_make("rtph264pay",    "pay");
    GstElement *sink      = gst_element_factory_make("udpsink",       "sink");

    if (!pipeline || !source || !demux || !parse || !pay || !sink) {
        std::cerr << "[TX] ERROR: Failed to create file pipeline elements." << std::endl;
        if (pipeline) gst_object_unref(pipeline);
        return nullptr;
    }

    g_object_set(source, "location", path.c_str(), NULL);
    g_object_set(pay,  "pt",              RTP_PT, NULL);
    g_object_set(pay,  "config-interval", -1,     NULL);
    g_object_set(sink, "host", TX_HOST, NULL);
    g_object_set(sink, "port", TX_PORT, NULL);
    // sync=TRUE (default): pipeline clock throttles output to the file's native framerate.
    // sync=FALSE would send all frames as fast as disk allows, causing instant EOS.

    std::cout << "[TX] Mode: file (" << path << ") via " << demux_name << std::endl;

    gst_bin_add_many(GST_BIN(pipeline), source, demux, parse, pay, sink, NULL);

    // filesrc → demux (static link)
    if (!gst_element_link(source, demux)) {
        std::cerr << "[TX] ERROR: Failed to link filesrc → demux." << std::endl;
        gst_object_unref(pipeline);
        return nullptr;
    }

    // demux → parse: dynamic pad, wired up in on_pad_added
    g_signal_connect(demux, "pad-added", G_CALLBACK(on_pad_added), parse);

    // parse → pay → sink (static)
    if (!gst_element_link_many(parse, pay, sink, NULL)) {
        std::cerr << "[TX] ERROR: Failed to link parse → pay → sink." << std::endl;
        gst_object_unref(pipeline);
        return nullptr;
    }

    return pipeline;
}

static void run_loop(GstElement *pipeline)
{
    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "[TX] ERROR: Failed to set pipeline to PLAYING." << std::endl;
        return;
    }

    std::cout << "[TX] Sending to " << TX_HOST << ":" << TX_PORT << std::endl;
    std::cout << "[TX] Pipeline running. Press Ctrl+C to stop." << std::endl;

    GstBus *bus = gst_element_get_bus(pipeline);

    while (true) {
        GstMessage *msg = gst_bus_timed_pop_filtered(
            bus,
            100 * GST_MSECOND,
            (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS | GST_MESSAGE_STATE_CHANGED)
        );

        if (!msg) continue;

        switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_EOS:
                std::cout << "[TX] EOS." << std::endl;
                gst_message_unref(msg);
                goto done;

            case GST_MESSAGE_ERROR: {
                GError *err = NULL;
                gchar  *dbg = NULL;
                gst_message_parse_error(msg, &err, &dbg);
                std::cerr << "[TX] ERROR: " << err->message << std::endl;
                if (dbg) { std::cerr << "[TX] Debug: " << dbg << std::endl; g_free(dbg); }
                g_error_free(err);
                gst_message_unref(msg);
                goto done;
            }

            case GST_MESSAGE_STATE_CHANGED:
                if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline)) {
                    GstState old_s, new_s;
                    gst_message_parse_state_changed(msg, &old_s, &new_s, NULL);
                    std::cout << "[TX] State: "
                              << gst_element_state_get_name(old_s) << " → "
                              << gst_element_state_get_name(new_s)  << std::endl;
                }
                gst_message_unref(msg);
                break;

            default:
                gst_message_unref(msg);
                break;
        }
    }

done:
    std::cout << "[TX] Shutting down..." << std::endl;
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    std::cout << "[TX] Initializing GStreamer..." << std::endl;

    GstElement *pipeline = nullptr;

    if (argc >= 3 && std::string(argv[1]) == "--file") {
        pipeline = build_file_pipeline(argv[2]);
    } else {
        std::string device = (argc >= 2) ? argv[1] : "/dev/video0";
        // build_file_pipeline-style but inline for webcam
        GstElement *pl    = gst_pipeline_new("tx-pipeline");
        GstElement *src   = gst_element_factory_make("v4l2src",      "source");
        GstElement *cf    = gst_element_factory_make("capsfilter",   "capsfilter");
        GstElement *conv  = gst_element_factory_make("videoconvert", "convert");
        GstElement *ef    = gst_element_factory_make("capsfilter",   "encfilter");
        GstElement *enc   = gst_element_factory_make("x264enc",      "encoder");
        GstElement *pay   = gst_element_factory_make("rtph264pay",   "pay");
        GstElement *sink  = gst_element_factory_make("udpsink",      "sink");

        if (!pl || !src || !cf || !conv || !ef || !enc || !pay || !sink) {
            std::cerr << "[TX] ERROR: Failed to create webcam pipeline elements." << std::endl;
            return -1;
        }

        g_object_set(src, "device", device.c_str(), NULL);

        GstCaps *caps = gst_caps_new_simple("video/x-raw",
            "format",    G_TYPE_STRING,     "YUY2",
            "width",     G_TYPE_INT,        640,
            "height",    G_TYPE_INT,        480,
            "framerate", GST_TYPE_FRACTION, 30, 1,
            NULL);
        g_object_set(cf, "caps", caps, NULL);
        gst_caps_unref(caps);

        GstCaps *i420 = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "I420", NULL);
        g_object_set(ef, "caps", i420, NULL);
        gst_caps_unref(i420);

        g_object_set(enc,  "tune",         0x00000004, NULL);
        g_object_set(enc,  "speed-preset", 1,          NULL);
        g_object_set(enc,  "key-int-max",  60,         NULL);
        g_object_set(pay,  "pt",              RTP_PT,   NULL);
        g_object_set(pay,  "config-interval", -1,       NULL);
        g_object_set(sink, "host",  TX_HOST,  NULL);
        g_object_set(sink, "port",  TX_PORT,  NULL);
        g_object_set(sink, "sync",  FALSE,    NULL);
        g_object_set(sink, "async", FALSE,    NULL);

        std::cout << "[TX] Mode: webcam (" << device << ")" << std::endl;

        gst_bin_add_many(GST_BIN(pl), src, cf, conv, ef, enc, pay, sink, NULL);
        if (!gst_element_link_many(src, cf, conv, ef, enc, pay, sink, NULL)) {
            std::cerr << "[TX] ERROR: Failed to link webcam pipeline." << std::endl;
            gst_object_unref(pl);
            return -1;
        }
        pipeline = pl;
    }

    if (!pipeline) return -1;

    run_loop(pipeline);
    gst_object_unref(pipeline);
    std::cout << "[TX] Done." << std::endl;
    return 0;
}
