/** 
 *  tests/test-recorder
 *
 *  Copyright 2019 SK Telecom Co., Ltd.
 *    Author: Jakub Adam <jakub.adam@collabora.com>
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

#include <gaeguli/gaeguli.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <gst/pbutils/pbutils.h>

#include "hwangsae/hwangsae.h"

typedef struct
{
  GMainLoop *loop;
  GaeguliFifoTransmit *transmit;
  GaeguliPipeline *pipeline;
  HwangsaeRecorder *recorder;

  gboolean should_stream;
  GThread *streaming_thread;
} TestFixture;

static void
fixture_setup (TestFixture * fixture, gconstpointer unused)
{
  g_autoptr (GError) error = NULL;

  fixture->loop = g_main_loop_new (NULL, FALSE);
  fixture->transmit = gaeguli_fifo_transmit_new ();
  fixture->pipeline =
      gaeguli_pipeline_new_full (GAEGULI_VIDEO_SOURCE_VIDEOTESTSRC, NULL,
      GAEGULI_ENCODING_METHOD_GENERAL);
  fixture->recorder = hwangsae_recorder_new ();
  g_object_set (fixture->recorder, "recording-dir", "/tmp", NULL);

  g_object_set (fixture->pipeline, "clock-overlay", TRUE, NULL);

  gaeguli_pipeline_add_fifo_target_full (fixture->pipeline,
      GAEGULI_VIDEO_CODEC_H264, GAEGULI_VIDEO_RESOLUTION_640X480,
      gaeguli_fifo_transmit_get_fifo (fixture->transmit), &error);
  g_assert_no_error (error);
}

static void
fixture_teardown (TestFixture * fixture, gconstpointer unused)
{
  g_clear_object (&fixture->recorder);
  g_clear_object (&fixture->transmit);
  g_clear_object (&fixture->pipeline);
  g_clear_pointer (&fixture->loop, g_main_loop_unref);
}

static gboolean
streaming_thread_func (TestFixture * fixture)
{
  g_autoptr (GMainContext) context = g_main_context_new ();
  g_autoptr (GError) error = NULL;
  guint transmit_id;

  g_main_context_push_thread_default (context);

  transmit_id = gaeguli_fifo_transmit_start (fixture->transmit,
      "127.0.0.1", 8888, GAEGULI_SRT_MODE_LISTENER, &error);
  g_assert_no_error (error);

  while (fixture->should_stream) {
    g_main_context_iteration (context, TRUE);
  }

  gaeguli_fifo_transmit_stop (fixture->transmit, transmit_id, &error);
  g_assert_no_error (error);

  return TRUE;
}

static void
start_streaming (TestFixture * fixture)
{
  g_assert_null (fixture->streaming_thread);

  fixture->should_stream = TRUE;
  fixture->streaming_thread = g_thread_new ("streaming_thread_func",
      (GThreadFunc) streaming_thread_func, fixture);
}

static void
stop_streaming (TestFixture * fixture)
{
  g_assert_nonnull (fixture->streaming_thread);

  fixture->should_stream = FALSE;
  g_clear_pointer (&fixture->streaming_thread, g_thread_join);
}

static GstClockTime
get_file_duration (const gchar * file_path)
{
  g_autoptr (GstDiscoverer) discoverer = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GstDiscovererInfo) info = NULL;
  g_autoptr (GstDiscovererStreamInfo) stream_info = NULL;
  g_autoptr (GstCaps) stream_caps = NULL;
  g_autofree gchar *stream_caps_str = NULL;
  g_autofree gchar *uri = NULL;
  const gchar *container_type;

  discoverer = gst_discoverer_new (5 * GST_SECOND, &error);
  g_assert_no_error (error);

  uri = g_strdup_printf ("file://%s", file_path);

  info = gst_discoverer_discover_uri (discoverer, uri, &error);
  g_assert_no_error (error);
  g_assert_cmpint (gst_discoverer_info_get_result (info), ==,
      GST_DISCOVERER_OK);

  stream_info = gst_discoverer_info_get_stream_info (info);
  stream_caps = gst_discoverer_stream_info_get_caps (stream_info);

  stream_caps_str = gst_caps_to_string (stream_caps);
  g_debug ("Container file has caps: %s", stream_caps_str);

  g_assert_cmpint (gst_caps_get_size (stream_caps), ==, 1);

  if (g_str_has_suffix (file_path, ".mp4")) {
    container_type = "video/quicktime";
  } else if (g_str_has_suffix (file_path, ".ts")) {
    container_type = "video/mpegts";
  } else {
    g_assert_not_reached ();
  }

  g_assert_cmpstr
      (gst_structure_get_name (gst_caps_get_structure (stream_caps, 0)), ==,
      container_type);

  return gst_discoverer_info_get_duration (info);
}

// recorder-record -------------------------------------------------------------

typedef struct
{
  TestFixture *fixture;
  gboolean got_file_created_signal;
  gboolean got_file_completed_signal;
} RecorderTestData;

static gboolean
stop_recording_timeout_cb (TestFixture * fixture)
{
  hwangsae_recorder_stop_recording (fixture->recorder);

  return G_SOURCE_REMOVE;
}

static void
stream_connected_cb (HwangsaeRecorder * recorder, TestFixture * fixture)
{
  g_debug ("Stream connected");
  g_timeout_add_seconds (5, (GSourceFunc) stop_recording_timeout_cb, fixture);
}

static void
file_created_cb (HwangsaeRecorder * recorder, const gchar * file_path,
    RecorderTestData * data)
{
  g_debug ("File %s created", file_path);

  g_assert_false (data->got_file_created_signal);
  data->got_file_created_signal = TRUE;
}

static void
file_completed_cb (HwangsaeRecorder * recorder, const gchar * file_path,
    RecorderTestData * data)
{
  GstClockTime duration = get_file_duration (file_path);

  g_debug ("Finished recording %s, duration %" GST_TIME_FORMAT, file_path,
      GST_TIME_ARGS (duration));

  g_assert_cmpint (labs (GST_CLOCK_DIFF (duration, 5 * GST_SECOND)), <=,
      GST_SECOND);

  g_assert_false (data->got_file_completed_signal);
  data->got_file_completed_signal = TRUE;
}

static void
stream_disconnected_cb (HwangsaeRecorder * recorder, TestFixture * fixture)
{
  g_debug ("Stream disconnected");

  gaeguli_pipeline_stop (fixture->pipeline);
  stop_streaming (fixture);

  g_main_loop_quit (fixture->loop);
}

static void
test_hwangsae_recorder_record (TestFixture * fixture, gconstpointer data)
{
  HwangsaeContainer container = GPOINTER_TO_INT (data);
  RecorderTestData test_data = { 0 };
  g_autoptr (GError) error = NULL;

  test_data.fixture = fixture;

  hwangsae_recorder_set_container (fixture->recorder, container);

  g_signal_connect (fixture->recorder, "stream-connected",
      (GCallback) stream_connected_cb, fixture);
  g_signal_connect (fixture->recorder, "file-created",
      (GCallback) file_created_cb, &test_data);
  g_signal_connect (fixture->recorder, "file-completed",
      (GCallback) file_completed_cb, &test_data);
  g_signal_connect (fixture->recorder, "stream-disconnected",
      (GCallback) stream_disconnected_cb, fixture);

  start_streaming (fixture);

  hwangsae_recorder_start_recording (fixture->recorder, "srt://127.0.0.1:8888");

  g_main_loop_run (fixture->loop);

  g_assert_true (test_data.got_file_created_signal);
  g_assert_true (test_data.got_file_completed_signal);
}

// recorder-disconnect ---------------------------------------------------------

const guint SEGMENT_LEN_SECONDS = 5;

typedef struct
{
  GMainLoop *loop;
  gboolean has_initial_segment;
  GstClockTime gap_start;
  GstClockTime gap_end;
} CheckGapsData;

static GstPadProbeReturn
gap_probe_cb (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  CheckGapsData *data = user_data;

  if (GST_IS_EVENT (info->data)) {
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);

    switch (GST_EVENT_TYPE (event)) {
      case GST_EVENT_SEGMENT:{
        if (data->has_initial_segment) {
          const GstSegment *segment;

          gst_event_parse_segment (event, &segment);
          g_debug ("Segment event at %" GST_TIME_FORMAT,
              GST_TIME_ARGS (segment->position));

          g_assert_cmpuint (data->gap_start, ==, GST_CLOCK_TIME_NONE);
          data->gap_start = segment->position;
        } else {
          // Ignore the segment event at the beginning of the recording.
          data->has_initial_segment = TRUE;
        }
        break;
      }
      case GST_EVENT_EOS:
        g_main_loop_quit (data->loop);
        break;
      default:
        break;
    }
  } else if (GST_IS_BUFFER (info->data)) {
    if (data->gap_start != GST_CLOCK_TIME_NONE &&
        data->gap_end == GST_CLOCK_TIME_NONE) {
      GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER (info);
      data->gap_end = GST_BUFFER_PTS (buffer);
    }
  }

  return GST_PAD_PROBE_OK;
}

static GstClockTimeDiff
get_gap_duration (const gchar * file_path)
{
  g_autoptr (GMainContext) context = g_main_context_new ();
  g_autoptr (GMainLoop) loop = g_main_loop_new (context, FALSE);
  g_autoptr (GstElement) pipeline = NULL;
  g_autoptr (GstElement) sink = NULL;
  g_autoptr (GstPad) pad = NULL;
  g_autofree gchar *pipeline_str = NULL;
  CheckGapsData data = { 0 };
  g_autoptr (GError) error = NULL;

  g_main_context_push_thread_default (context);

  pipeline_str =
      g_strdup_printf ("filesrc location=%s ! decodebin ! fakesink name=sink",
      file_path);

  pipeline = gst_parse_launch (pipeline_str, &error);
  g_assert_no_error (error);

  sink = gst_bin_get_by_name (GST_BIN (pipeline), "sink");
  g_assert_nonnull (sink);
  pad = gst_element_get_static_pad (sink, "sink");
  g_assert_nonnull (pad);

  data.loop = loop;
  data.gap_start = data.gap_end = GST_CLOCK_TIME_NONE;
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_DATA_DOWNSTREAM, gap_probe_cb,
      &data, NULL);

  gst_element_set_state (pipeline, GST_STATE_PLAYING);

  g_main_loop_run (loop);

  gst_element_set_state (pipeline, GST_STATE_NULL);

  g_main_context_pop_thread_default (context);

  return GST_CLOCK_DIFF (data.gap_start, data.gap_end);
}

static void
recording_done_cb (HwangsaeRecorder * recorder, const gchar * file_path,
    TestFixture * fixture)
{
  GstClockTime duration;
  GstClockTimeDiff gap;
  const GstClockTime expected_duration = 3 * SEGMENT_LEN_SECONDS * GST_SECOND;
  const GstClockTime expected_gap = SEGMENT_LEN_SECONDS * GST_SECOND;

  gaeguli_pipeline_stop (fixture->pipeline);
  stop_streaming (fixture);

  duration = get_file_duration (file_path);

  g_debug ("Finished recording %s, duration %" GST_TIME_FORMAT, file_path,
      GST_TIME_ARGS (duration));

  g_assert_cmpint (labs (GST_CLOCK_DIFF (duration, expected_duration)), <=,
      GST_SECOND);

  gap = get_gap_duration (file_path);

  g_debug ("Gap in the file lasts %" GST_STIME_FORMAT, GST_STIME_ARGS (gap));

  g_assert_cmpint (labs (GST_CLOCK_DIFF (gap, expected_gap)), <=, GST_SECOND);
}

static gboolean
second_segment_done_cb (TestFixture * fixture)
{
  g_debug ("Second segment done.");
  hwangsae_recorder_stop_recording (fixture->recorder);

  return G_SOURCE_REMOVE;
}

static gboolean
second_segment_started_cb (TestFixture * fixture)
{
  g_debug ("Recording second segment of %u seconds.", SEGMENT_LEN_SECONDS);
  start_streaming (fixture);
  g_timeout_add_seconds (SEGMENT_LEN_SECONDS,
      (GSourceFunc) second_segment_done_cb, fixture);

  return G_SOURCE_REMOVE;
}

static gboolean
first_segment_done_cb (TestFixture * fixture)
{
  g_debug ("First segment done. Stopping streaming for %u seconds.",
      SEGMENT_LEN_SECONDS);
  stop_streaming (fixture);
  g_timeout_add_seconds (SEGMENT_LEN_SECONDS,
      (GSourceFunc) second_segment_started_cb, fixture);

  return G_SOURCE_REMOVE;
}

static void
first_segment_started_cb (HwangsaeRecorder * recorder, TestFixture * fixture)
{
  g_debug ("Recording first segment of 5 seconds.");
  g_timeout_add_seconds (SEGMENT_LEN_SECONDS,
      (GSourceFunc) first_segment_done_cb, fixture);
}

static void
test_hwangsae_recorder_disconnect (TestFixture * fixture, gconstpointer unused)
{
  g_signal_connect (fixture->recorder, "stream-connected",
      (GCallback) first_segment_started_cb, fixture);
  g_signal_connect (fixture->recorder, "file-completed",
      (GCallback) recording_done_cb, fixture);
  g_signal_connect_swapped (fixture->recorder, "stream-disconnected",
      (GCallback) g_main_loop_quit, fixture->loop);

  start_streaming (fixture);

  hwangsae_recorder_start_recording (fixture->recorder, "srt://127.0.0.1:8888");

  g_main_loop_run (fixture->loop);
}

// recorder-split --------------------------------------------------------------

const guint NUM_FILE_SEGMENTS = 3;

typedef struct
{
  GSList *filenames;
  guint file_created_signal_count;
  guint file_completed_signal_count;
} SplitData;

static void
split_file_created_cb (HwangsaeRecorder * recorder,
    const gchar * file_path, SplitData * data)
{
  g_debug ("Created file %s", file_path);

  data->filenames = g_slist_append (data->filenames, g_strdup (file_path));
  if (++data->file_created_signal_count == NUM_FILE_SEGMENTS) {
    hwangsae_recorder_stop_recording (recorder);
  }
}

static void
split_file_completed_cb (HwangsaeRecorder * recorder,
    const gchar * file_path, SplitData * data)
{
  g_debug ("Completed file %s", file_path);

  ++data->file_completed_signal_count;
}

static GSList *
split_run_test (TestFixture * fixture)
{
  SplitData data = { 0 };

  g_signal_connect (fixture->recorder, "file-created",
      (GCallback) split_file_created_cb, &data);
  g_signal_connect (fixture->recorder, "file-completed",
      (GCallback) split_file_completed_cb, &data);
  g_signal_connect (fixture->recorder, "stream-disconnected",
      (GCallback) stream_disconnected_cb, fixture);

  start_streaming (fixture);

  hwangsae_recorder_start_recording (fixture->recorder, "srt://127.0.0.1:8888");

  g_main_loop_run (fixture->loop);

  g_assert_cmpint (data.file_created_signal_count, >=, NUM_FILE_SEGMENTS);
  g_assert_cmpint (data.file_completed_signal_count, ==,
      data.file_created_signal_count);
  g_assert_cmpint (g_slist_length (data.filenames), >=, NUM_FILE_SEGMENTS);

  return data.filenames;
}

static void
test_hwangsae_recorder_split_time (TestFixture * fixture, gconstpointer unused)
{
  GSList *filenames;
  const GstClockTimeDiff FILE_SEGMENT_LEN = 5 * GST_SECOND;

  hwangsae_recorder_set_max_size_time (fixture->recorder, FILE_SEGMENT_LEN);

  filenames = split_run_test (fixture);

  for (; filenames; filenames = g_slist_delete_link (filenames, filenames)) {
    g_autofree gchar *filename = filenames->data;
    GstClockTime duration = get_file_duration (filename);

    g_debug ("%s has duration %" GST_TIME_FORMAT, filename,
        GST_TIME_ARGS (duration));

    if (filenames->next) {
      g_assert_cmpint (labs (GST_CLOCK_DIFF (duration, FILE_SEGMENT_LEN)), <=,
          GST_SECOND);
    } else {
      /* The final segment should be shorter than FILE_SEGMENT_LEN. */
      g_assert_cmpint (duration, <, FILE_SEGMENT_LEN);
    }
  }
}

static void
test_hwangsae_recorder_split_bytes (TestFixture * fixture, gconstpointer unused)
{
  GSList *filenames;
  guint64 FILE_SEGMENT_LEN_BYTES = 5e6;

  hwangsae_recorder_set_max_size_bytes (fixture->recorder,
      FILE_SEGMENT_LEN_BYTES);

  filenames = split_run_test (fixture);

  for (; filenames; filenames = g_slist_delete_link (filenames, filenames)) {
    g_autofree gchar *filename = filenames->data;
    GStatBuf stat_buf;

    g_assert (g_stat (filename, &stat_buf) == 0);

    g_debug ("%s has size %luB", filename, stat_buf.st_size);

    if (filenames->next) {
      g_assert_cmpint (labs (stat_buf.st_size - FILE_SEGMENT_LEN_BYTES), <=,
          FILE_SEGMENT_LEN_BYTES / 5);
    } else {
      /* The final segment should be shorter than FILE_SEGMENT_LEN_BYTES. */
      g_assert_cmpint (stat_buf.st_size, <, FILE_SEGMENT_LEN_BYTES);
    }
  }
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  /* Don't treat warnings as fatal, which is GTest default. */
  g_log_set_always_fatal (G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);

  g_test_add ("/hwangsae/recorder-record-mp4",
      TestFixture, GUINT_TO_POINTER (HWANGSAE_CONTAINER_MP4), fixture_setup,
      test_hwangsae_recorder_record, fixture_teardown);

  g_test_add ("/hwangsae/recorder-record-ts",
      TestFixture, GUINT_TO_POINTER (HWANGSAE_CONTAINER_TS), fixture_setup,
      test_hwangsae_recorder_record, fixture_teardown);

  g_test_add ("/hwangsae/recorder-disconnect",
      TestFixture, NULL, fixture_setup,
      test_hwangsae_recorder_disconnect, fixture_teardown);

  g_test_add ("/hwangsae/recorder-split-time",
      TestFixture, NULL, fixture_setup,
      test_hwangsae_recorder_split_time, fixture_teardown);

  g_test_add ("/hwangsae/recorder-split-bytes",
      TestFixture, NULL, fixture_setup,
      test_hwangsae_recorder_split_bytes, fixture_teardown);

  return g_test_run ();
}
