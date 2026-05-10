/**
 * tx.cpp - GStreamer RTP Transmitter (Loopback Test)
 *
 * Reads an H264 video file, wraps it in RTP, and sends over UDP to localhost.
 *
 * Pipeline:
 *   filesrc → qtdemux → h264parse → rtph264pay → udpsink
 *
 * Build:
 *   g++ tx.cpp -o tx $(pkg-config --cflags --libs gstreamer-1.0)
 *
 * Run:
 *   ./tx video.mp4
 */

#include <gst/gst.h>
#include <iostream>
#include <string>

#define TX_HOST  "127.0.0.1"
#define TX_PORT  5000
#define RTP_PT   96

// qtdemux has a dynamic src pad — link it to h264parse when the video pad appears
static void on_pad_added(GstElement*, GstPad *pad, gpointer data)
{
    GstCaps *caps = gst_pad_get_current_caps(pad);
    const gchar *name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
    gst_caps_unref(caps);

    if (!g_str_has_prefix(name, "video/x-h264"))
        return;

    GstElement *parser = (GstElement *)data;
    GstPad *sinkpad = gst_element_get_static_pad(parser, "sink");
    if (!gst_pad_is_linked(sinkpad))
        gst_pad_link(pad, sinkpad);
    gst_object_unref(sinkpad);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        std::cerr << "Usage: ./tx <path_to_video.mp4>" << std::endl;
        return -1;
    }

    std::string filepath = argv[1];

    // -------------------------------------------------------------------------
    // Initialize GStreamer
    // -------------------------------------------------------------------------
    gst_init(&argc, &argv);

    std::cout << "[TX] Initializing GStreamer..." << std::endl;

    // -------------------------------------------------------------------------
    // Create pipeline elements
    // -------------------------------------------------------------------------
    GstElement *pipeline = gst_pipeline_new("tx-pipeline");
    GstElement *source   = gst_element_factory_make("filesrc",      "source");
    GstElement *demux    = gst_element_factory_make("qtdemux",      "demux");
    GstElement *parser   = gst_element_factory_make("h264parse",    "parser");
    GstElement *pay      = gst_element_factory_make("rtph264pay",   "pay");
    GstElement *sink     = gst_element_factory_make("udpsink",      "sink");

    if (!pipeline || !source || !demux || !parser || !pay || !sink) {
        std::cerr << "[TX] ERROR: Failed to create one or more elements." << std::endl;
        std::cerr << "[TX] Make sure gstreamer1.0-plugins-good is installed." << std::endl;
        return -1;
    }

    // -------------------------------------------------------------------------
    // Configure elements
    // -------------------------------------------------------------------------

    // filesrc: point to the input video file
    g_object_set(source, "location", filepath.c_str(), NULL);

    // rtph264pay: set payload type and send SPS/PPS with every keyframe
    g_object_set(pay, "pt", RTP_PT, NULL);
    g_object_set(pay, "config-interval", -1, NULL);

    // udpsink: send to localhost on TX_PORT
    g_object_set(sink, "host", TX_HOST, NULL);
    g_object_set(sink, "port", TX_PORT, NULL);
    g_object_set(sink, "sync", TRUE, NULL);  // sync to clock so we don't blast too fast

    std::cout << "[TX] Sending to " << TX_HOST << ":" << TX_PORT << std::endl;
    std::cout << "[TX] Source file: " << filepath << std::endl;

    // -------------------------------------------------------------------------
    // Add elements to pipeline and link
    // -------------------------------------------------------------------------
    gst_bin_add_many(GST_BIN(pipeline), source, demux, parser, pay, sink, NULL);

    // filesrc → qtdemux (static link)
    if (!gst_element_link(source, demux)) {
        std::cerr << "[TX] ERROR: Failed to link filesrc → qtdemux." << std::endl;
        gst_object_unref(pipeline);
        return -1;
    }

    // qtdemux → h264parse (dynamic pad, handled in callback)
    g_signal_connect(demux, "pad-added", G_CALLBACK(on_pad_added), parser);

    // h264parse → rtph264pay → udpsink (static link)
    if (!gst_element_link_many(parser, pay, sink, NULL)) {
        std::cerr << "[TX] ERROR: Failed to link parser → pay → sink." << std::endl;
        gst_object_unref(pipeline);
        return -1;
    }

    // -------------------------------------------------------------------------
    // Start the pipeline
    // -------------------------------------------------------------------------
    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "[TX] ERROR: Failed to set pipeline to PLAYING." << std::endl;
        gst_object_unref(pipeline);
        return -1;
    }

    std::cout << "[TX] Pipeline running. Press Ctrl+C to stop." << std::endl;

    // -------------------------------------------------------------------------
    // Main loop — wait for EOS or error
    // -------------------------------------------------------------------------
    GstBus *bus = gst_element_get_bus(pipeline);

    while (true) {
        GstMessage *msg = gst_bus_timed_pop_filtered(
            bus,
            100 * GST_MSECOND,
            (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS | GST_MESSAGE_STATE_CHANGED)
        );

        if (msg == NULL) continue;

        switch (GST_MESSAGE_TYPE(msg)) {

            case GST_MESSAGE_EOS:
                std::cout << "[TX] End of stream reached." << std::endl;
                gst_message_unref(msg);
                goto cleanup;

            case GST_MESSAGE_ERROR: {
                GError *err = NULL;
                gchar  *dbg = NULL;
                gst_message_parse_error(msg, &err, &dbg);
                std::cerr << "[TX] ERROR: " << err->message << std::endl;
                if (dbg) {
                    std::cerr << "[TX] Debug: " << dbg << std::endl;
                    g_free(dbg);
                }
                g_error_free(err);
                gst_message_unref(msg);
                goto cleanup;
            }

            case GST_MESSAGE_STATE_CHANGED: {
                if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline)) {
                    GstState old_state, new_state;
                    gst_message_parse_state_changed(msg, &old_state, &new_state, NULL);
                    std::cout << "[TX] State: "
                              << gst_element_state_get_name(old_state)
                              << " → "
                              << gst_element_state_get_name(new_state)
                              << std::endl;
                }
                gst_message_unref(msg);
                break;
            }

            default:
                gst_message_unref(msg);
                break;
        }
    }

cleanup:
    std::cout << "[TX] Shutting down..." << std::endl;
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    std::cout << "[TX] Done." << std::endl;
    return 0;
}
