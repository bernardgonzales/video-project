/**
 * rx.cpp - GStreamer RTP Receiver (Loopback Test)
 *
 * Receives an RTP H264 stream over UDP, decodes it, and displays it.
 *
 * Pipeline:
 *   udpsrc → rtph264depay → h264parse → avdec_h264 → videoconvert → autovideosink
 *
 * Build:
 *   g++ rx.cpp -o rx $(pkg-config --cflags --libs gstreamer-1.0)
 *
 * Run:
 *   ./rx
 *
 * Note: Start rx before tx so no packets are missed.
 */

#include <gst/gst.h>
#include <iostream>

#define RX_PORT  5000
#define RTP_PT   96

// RTP caps that tell udpsrc what kind of stream to expect
#define RTP_CAPS \
    "application/x-rtp,"       \
    "media=video,"              \
    "encoding-name=H264,"       \
    "payload=" G_STRINGIFY(RTP_PT)

int main(int argc, char *argv[])
{
    // -------------------------------------------------------------------------
    // Initialize GStreamer
    // -------------------------------------------------------------------------
    gst_init(&argc, &argv);

    std::cout << "[RX] Initializing GStreamer..." << std::endl;

    // -------------------------------------------------------------------------
    // Create pipeline elements
    // -------------------------------------------------------------------------
    GstElement *pipeline = gst_pipeline_new("rx-pipeline");
    GstElement *source   = gst_element_factory_make("udpsrc",        "source");
    GstElement *depay    = gst_element_factory_make("rtph264depay",  "depay");
    GstElement *parser   = gst_element_factory_make("h264parse",     "parser");
    GstElement *decoder  = gst_element_factory_make("avdec_h264",    "decoder");
    GstElement *convert  = gst_element_factory_make("videoconvert",  "convert");
    GstElement *sink     = gst_element_factory_make("ximagesink",    "sink");

    if (!pipeline || !source || !depay || !parser || !decoder || !convert || !sink) {
        std::cerr << "[RX] ERROR: Failed to create one or more elements." << std::endl;
        std::cerr << "[RX] Make sure the following are installed:" << std::endl;
        std::cerr << "[RX]   gstreamer1.0-plugins-good  (udpsrc, rtph264depay)" << std::endl;
        std::cerr << "[RX]   gstreamer1.0-libav          (avdec_h264)" << std::endl;
        std::cerr << "[RX]   gstreamer1.0-plugins-base   (videoconvert, ximagesink)" << std::endl;
        return -1;
    }

    // -------------------------------------------------------------------------
    // Configure elements
    // -------------------------------------------------------------------------

    // udpsrc: listen on RX_PORT and set RTP caps so it knows what's coming
    g_object_set(source, "port", RX_PORT, NULL);

    GstElement *queue = gst_element_factory_make("queue", "queue");

    GstCaps *caps = gst_caps_from_string(RTP_CAPS);
    g_object_set(source, "caps", caps, NULL);
    gst_caps_unref(caps);

    // autovideosink: don't block state transitions (live source) and don't sync to clock
    g_object_set(sink, "sync", FALSE, NULL);
    g_object_set(sink, "async", FALSE, NULL);

    std::cout << "[RX] Listening on port " << RX_PORT << std::endl;
    std::cout << "[RX] Waiting for stream..." << std::endl;

    // -------------------------------------------------------------------------
    // Add elements to pipeline and link
    // -------------------------------------------------------------------------
    gst_bin_add_many(GST_BIN(pipeline),
    source, queue, depay, parser, decoder, convert, sink, NULL);


    if (!gst_element_link_many(source, queue, depay, parser, decoder, convert, sink, NULL)) {
        std::cerr << "[RX] ERROR: Failed to link pipeline elements." << std::endl;
        gst_object_unref(pipeline);
        return -1;
    }

    // -------------------------------------------------------------------------
    // Start the pipeline
    // -------------------------------------------------------------------------
    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "[RX] ERROR: Failed to set pipeline to PLAYING." << std::endl;
        gst_object_unref(pipeline);
        return -1;
    }

    std::cout << "[RX] Pipeline running. Press Ctrl+C to stop." << std::endl;

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
                std::cout << "[RX] End of stream." << std::endl;
                gst_message_unref(msg);
                goto cleanup;

            case GST_MESSAGE_ERROR: {
                GError *err = NULL;
                gchar  *dbg = NULL;
                gst_message_parse_error(msg, &err, &dbg);
                std::cerr << "[RX] ERROR: " << err->message << std::endl;
                if (dbg) {
                    std::cerr << "[RX] Debug: " << dbg << std::endl;
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
                    std::cout << "[RX] State: "
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
    // -------------------------------------------------------------------------
    // Cleanup
    // -------------------------------------------------------------------------
    std::cout << "[RX] Shutting down..." << std::endl;
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    std::cout << "[RX] Done." << std::endl;
    return 0;
}
