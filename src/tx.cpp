/**
 * tx.cpp - GStreamer RTP Transmitter (Webcam)
 *
 * Captures from a V4L2 webcam, encodes to H264, wraps in RTP, and sends over UDP.
 *
 * Pipeline:
 *   v4l2src → capsfilter (YUY2 640x480 30fps) → videoconvert → capsfilter (I420) → x264enc → rtph264pay → udpsink
 *
 * Build:
 *   g++ tx.cpp -o tx $(pkg-config --cflags --libs gstreamer-1.0)
 *
 * Run:
 *   ./tx [/dev/videoN]   (defaults to /dev/video0)
 */

#include <gst/gst.h>
#include <iostream>
#include <string>

#define TX_HOST  "127.0.0.1"
#define TX_PORT  5000
#define RTP_PT   96

int main(int argc, char *argv[])
{
    std::string device = (argc >= 2) ? argv[1] : "/dev/video0";

    // -------------------------------------------------------------------------
    // Initialize GStreamer
    // -------------------------------------------------------------------------
    gst_init(&argc, &argv);

    std::cout << "[TX] Initializing GStreamer..." << std::endl;

    // -------------------------------------------------------------------------
    // Create pipeline elements
    // -------------------------------------------------------------------------
    GstElement *pipeline  = gst_pipeline_new("tx-pipeline");
    GstElement *source    = gst_element_factory_make("v4l2src",      "source");
    GstElement *capsfilter= gst_element_factory_make("capsfilter",   "capsfilter");
    GstElement *convert   = gst_element_factory_make("videoconvert", "convert");
    GstElement *encfilter = gst_element_factory_make("capsfilter",   "encfilter");
    GstElement *encoder   = gst_element_factory_make("x264enc",      "encoder");
    GstElement *pay       = gst_element_factory_make("rtph264pay",   "pay");
    GstElement *sink      = gst_element_factory_make("udpsink",      "sink");

    if (!pipeline || !source || !capsfilter || !convert || !encfilter || !encoder || !pay || !sink) 
    {
        std::cerr << "[TX] ERROR: Failed to create one or more elements." << std::endl;
        std::cerr << "[TX] Make sure gstreamer1.0-plugins-good and gstreamer1.0-plugins-ugly are installed." << std::endl;
        return -1;
    }

    // -------------------------------------------------------------------------
    // Configure elements
    // -------------------------------------------------------------------------

    // v4l2src: webcam device
    g_object_set(source, "device", device.c_str(), NULL);

    // capsfilter: pin format so caps negotiation doesn't stall
    GstCaps *caps = gst_caps_new_simple("video/x-raw",
        "format",    G_TYPE_STRING, "YUY2",
        "width",     G_TYPE_INT,    640,
        "height",    G_TYPE_INT,    480,
        "framerate", GST_TYPE_FRACTION, 30, 1,
        NULL);
    g_object_set(capsfilter, "caps", caps, NULL);
    gst_caps_unref(caps);

    // encfilter: force I420 so x264enc uses the standard 4:2:0 high profile;
    // without this, videoconvert picks Y42B (4:2:2) which x264enc silently drops
    GstCaps *i420 = gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, "I420",
        NULL);
    g_object_set(encfilter, "caps", i420, NULL);
    gst_caps_unref(i420);

    // x264enc: zero-latency preset for live streaming; frequent keyframes so the
    // receiver can start decoding within ~2 seconds of connecting
    g_object_set(encoder, "tune",        0x00000004 /* zerolatency */, NULL);
    g_object_set(encoder, "speed-preset", 1 /* ultrafast */,           NULL);
    g_object_set(encoder, "key-int-max",  60,                          NULL);

    // rtph264pay: set payload type and send SPS/PPS with every keyframe
    g_object_set(pay, "pt", RTP_PT, NULL);
    g_object_set(pay, "config-interval", -1, NULL);

    // udpsink: send to localhost on TX_PORT
    // sync=FALSE, async=FALSE: live sources won't preroll, so don't let the sink
    // block the PAUSED→PLAYING transition waiting for a buffer that never comes
    g_object_set(sink, "host",  TX_HOST, NULL);
    g_object_set(sink, "port",  TX_PORT, NULL);
    g_object_set(sink, "sync",  FALSE,   NULL);
    g_object_set(sink, "async", FALSE,   NULL);

    std::cout << "[TX] Sending to " << TX_HOST << ":" << TX_PORT << std::endl;
    std::cout << "[TX] Webcam device: " << device << std::endl;

    // -------------------------------------------------------------------------
    // Add elements to pipeline and link
    // -------------------------------------------------------------------------
    gst_bin_add_many(GST_BIN(pipeline), source, capsfilter, convert, encfilter, encoder, pay, sink, NULL);

    // v4l2src → capsfilter → videoconvert → encfilter → x264enc → rtph264pay → udpsink
    if (!gst_element_link_many(source, capsfilter, convert, encfilter, encoder, pay, sink, NULL)) 
    {
        std::cerr << "[TX] ERROR: Failed to link pipeline elements." << std::endl;
        gst_object_unref(pipeline);
        return -1;
    }

    // -------------------------------------------------------------------------
    // Start the pipeline
    // -------------------------------------------------------------------------
    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) 
    {
        std::cerr << "[TX] ERROR: Failed to set pipeline to PLAYING." << std::endl;
        gst_object_unref(pipeline);
        return -1;
    }

    std::cout << "[TX] Pipeline running. Press Ctrl+C to stop." << std::endl;

    // -------------------------------------------------------------------------
    // Main loop — wait for EOS or error
    // -------------------------------------------------------------------------
    GstBus *bus = gst_element_get_bus(pipeline);

    while (true) 
    {
        GstMessage *msg = gst_bus_timed_pop_filtered(
            bus,
            100 * GST_MSECOND,
            (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS | GST_MESSAGE_STATE_CHANGED)
        );

        if (msg == NULL) continue;

        switch (GST_MESSAGE_TYPE(msg)) 
        {
            case GST_MESSAGE_EOS:
                std::cout << "[TX] Unexpected EOS." << std::endl;
                gst_message_unref(msg);
                goto cleanup;

            case GST_MESSAGE_ERROR: 
            {
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

            case GST_MESSAGE_STATE_CHANGED: 
            {
                if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline)) 
                {
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
