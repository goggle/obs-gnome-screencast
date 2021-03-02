/*
 * obs-gnome-mutter-screencast. OBS Studio source plugin.
 * Copyright (C) 2019-2020 Florian Zwoch <fzwoch@gmail.com>
 *
 * This file is part of obs-gnome-mutter-screencast.
 *
 * obs-gnome-mutter-screencast is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * obs-gnome-mutter-screencast is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with obs-gnome-mutter-screencast. If not, see <http://www.gnu.org/licenses/>.
 */

#include <obs/obs-module.h>
#include <obs/util/platform.h>
#include <gio/gio.h>
#include <gst/gst.h>
#include <gst/app/app.h>
#include <gst/video/video.h>

OBS_DECLARE_MODULE()

typedef struct {
	gchar connector[256];
	gchar monitor[256];
} plugs_t;

typedef struct {
	GstElement *pipe;
	gchar *session_path;
	obs_source_t *source;
	obs_data_t *settings;
	int64_t count;
	guint subscribe_id;
	plugs_t plugs[32];
	gint num_plugs;
} data_t;

static void update_plug_names(data_t *data)
{
	GError *err = NULL;

	memset(data->plugs, 0, sizeof(data->plugs));
	data->num_plugs = 0;

	GDBusConnection *dbus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
	if (err != NULL) {
		blog(LOG_ERROR, "Cannot connect to DBus: %s", err->message);
		g_error_free(err);

		goto fail;
	}

	GVariant *display_config = g_dbus_connection_call_sync(
		dbus, "org.gnome.Mutter.DisplayConfig",
		"/org/gnome/Mutter/DisplayConfig",
		"org.gnome.Mutter.DisplayConfig", "GetCurrentState", NULL, NULL,
		G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);

	if (err != NULL) {
		blog(LOG_ERROR, "Cannot call GetCurrentState() on DBus: %s",
		     err->message);
		g_error_free(err);

		goto fail;
	}

	GVariant *list;

	g_variant_get(
		display_config,
		"(u@a((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv})",
		NULL, &list, NULL, NULL);

	GVariantIter iter;
	g_variant_iter_init(&iter, list);

	gchar *connector;
	gchar *monitor;

	while (g_variant_iter_loop(&iter, "((ssss)a(siiddada{sv})a{sv})",
				   &connector, NULL, &monitor, NULL, NULL,
				   NULL)) {
		g_strlcpy(data->plugs[data->num_plugs].connector, connector,
			  sizeof(data->plugs[data->num_plugs].connector));
		g_strlcpy(data->plugs[data->num_plugs].monitor, monitor,
			  sizeof(data->plugs[data->num_plugs].monitor));

		data->num_plugs++;

		if (data->num_plugs >= sizeof(data->plugs) / sizeof(plugs_t)) {
			break;
		}
	}

	g_variant_unref(list);
	g_variant_unref(display_config);

fail:
	if (dbus != NULL)
		g_object_unref(dbus);
}

static const char *get_name(void *type_data)
{
	return "GNOME Mutter Screen Cast";
}

static gboolean bus_callback(GstBus *bus, GstMessage *message,
			     gpointer user_data)
{
	data_t *data = user_data;

	switch (GST_MESSAGE_TYPE(message)) {
	case GST_MESSAGE_EOS:
		obs_source_output_video(data->source, NULL);
		break;
	case GST_MESSAGE_ERROR: {
		GError *err;
		gst_message_parse_error(message, &err, NULL);
		blog(LOG_ERROR, "%s", err->message);
		g_error_free(err);
	}
		gst_element_set_state(data->pipe, GST_STATE_NULL);
		obs_source_output_video(data->source, NULL);
		break;
	default:
		break;
	}

	return TRUE;
}

static GstFlowReturn new_sample(GstAppSink *appsink, gpointer user_data)
{
	data_t *data = user_data;
	GstSample *sample = gst_app_sink_pull_sample(appsink);
	GstBuffer *buffer = gst_sample_get_buffer(sample);
	GstCaps *caps = gst_sample_get_caps(sample);
	GstMapInfo info;
	GstVideoInfo video_info;

	gst_video_info_from_caps(&video_info, caps);
	gst_buffer_map(buffer, &info, GST_MAP_READ);

	struct obs_source_frame frame = {};

	frame.width = video_info.width;
	frame.height = video_info.height;
	frame.linesize[0] = video_info.stride[0];
	frame.linesize[1] = video_info.stride[1];
	frame.linesize[2] = video_info.stride[2];
	frame.data[0] = info.data + video_info.offset[0];
	frame.data[1] = info.data + video_info.offset[1];
	frame.data[2] = info.data + video_info.offset[2];

	frame.timestamp = obs_data_get_bool(data->settings, "timestamps")
				  ? os_gettime_ns()
				  : data->count++;

	enum video_range_type range = VIDEO_RANGE_DEFAULT;
	switch (video_info.colorimetry.range) {
	case GST_VIDEO_COLOR_RANGE_0_255:
		range = VIDEO_RANGE_FULL;
		frame.full_range = 1;
		break;
	case GST_VIDEO_COLOR_RANGE_16_235:
		range = VIDEO_RANGE_PARTIAL;
		break;
	default:
		break;
	}

	enum video_colorspace cs = VIDEO_CS_DEFAULT;
	switch (video_info.colorimetry.matrix) {
	case GST_VIDEO_COLOR_MATRIX_BT709:
		cs = VIDEO_CS_709;
		break;
	case GST_VIDEO_COLOR_MATRIX_BT601:
		cs = VIDEO_CS_601;
		break;
	default:
		break;
	}

	video_format_get_parameters(cs, range, frame.color_matrix,
				    frame.color_range_min,
				    frame.color_range_max);

	switch (video_info.finfo->format) {
	case GST_VIDEO_FORMAT_I420:
		frame.format = VIDEO_FORMAT_I420;
		break;
	case GST_VIDEO_FORMAT_NV12:
		frame.format = VIDEO_FORMAT_NV12;
		break;
	case GST_VIDEO_FORMAT_BGRx:
		// we usually get BGRx, however the alpha channel is set.
		// why not just fall through and use it.
		//frame.format = VIDEO_FORMAT_BGRX;
		//break;
	case GST_VIDEO_FORMAT_BGRA:
		frame.format = VIDEO_FORMAT_BGRA;
		break;
	case GST_VIDEO_FORMAT_RGBx:
	case GST_VIDEO_FORMAT_RGBA:
		frame.format = VIDEO_FORMAT_RGBA;
		break;
	case GST_VIDEO_FORMAT_UYVY:
		frame.format = VIDEO_FORMAT_UYVY;
		break;
	case GST_VIDEO_FORMAT_YUY2:
		frame.format = VIDEO_FORMAT_YUY2;
		break;
	case GST_VIDEO_FORMAT_YVYU:
		frame.format = VIDEO_FORMAT_YVYU;
		break;
	default:
		frame.format = VIDEO_FORMAT_NONE;
		blog(LOG_ERROR, "Unknown video format: %s",
		     video_info.finfo->name);
		break;
	}

	obs_source_output_video(data->source, &frame);

	gst_buffer_unmap(buffer, &info);
	gst_sample_unref(sample);

	return GST_FLOW_OK;
}

static void dbus_stream_closed_cb(GDBusConnection *connection,
				  const gchar *sender_name,
				  const gchar *object_path,
				  const gchar *interface_name,
				  const gchar *signal_name,
				  GVariant *parameters, gpointer user_data)
{
	data_t *data = user_data;

	if (data->pipe) {
		gst_element_set_state(data->pipe, GST_STATE_NULL);

		GstBus *bus = gst_element_get_bus(data->pipe);
		gst_bus_remove_watch(bus);
		gst_object_unref(bus);

		gst_object_unref(data->pipe);
		data->pipe = NULL;
	}

	obs_source_output_video(data->source, NULL);
}

static void dbus_cb(GDBusConnection *connection, const gchar *sender_name,
		    const gchar *object_path, const gchar *interface_name,
		    const gchar *signal_name, GVariant *parameters,
		    gpointer user_data)
{
	GError *err = NULL;
	data_t *data = user_data;
	guint node_id = 0;

	g_variant_get(parameters, "(u)", &node_id, NULL);

	gchar *pipeline = g_strdup_printf(
		"pipewiresrc client-name=obs-studio path=%u ! video/x-raw ! "
		"appsink max-buffers=2 drop=true sync=false name=appsink",
		node_id);

	data->pipe = gst_parse_launch(pipeline, &err);
	g_free(pipeline);

	if (err != NULL) {
		blog(LOG_ERROR, "Error gst_parse_launch(): %s", err->message);
		g_error_free(err);

		return;
	}

	GstAppSinkCallbacks appsink_cbs = {NULL, NULL, new_sample};

	GstElement *appsink =
		gst_bin_get_by_name(GST_BIN(data->pipe), "appsink");
	gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &appsink_cbs, data,
				   NULL);
	gst_object_unref(appsink);

	GstBus *bus = gst_element_get_bus(data->pipe);
	gst_bus_add_watch(bus, bus_callback, data);
	gst_object_unref(bus);

	gst_element_set_state(data->pipe, GST_STATE_PLAYING);
}

static void start(data_t *data)
{
	GError *err = NULL;
	GVariant *stream_res = NULL;
	GVariant *session_res = NULL;

	data->count = 0;

	GDBusConnection *dbus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
	if (err != NULL) {
		blog(LOG_ERROR, "Cannot connect to DBus: %s", err->message);
		g_error_free(err);

		goto fail;
	}

	session_res = g_dbus_connection_call_sync(
		dbus, "org.gnome.Mutter.ScreenCast",
		"/org/gnome/Mutter/ScreenCast", "org.gnome.Mutter.ScreenCast",
		"CreateSession", g_variant_new_parsed("({'dummy' : <0>},)"),
		NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);

	if (err != NULL) {
		blog(LOG_ERROR, "Cannot call CreateSession() on DBus: %s",
		     err->message);
		g_error_free(err);

		goto fail;
	}

	gchar *session_path;
	g_variant_get(session_res, "(o)", &session_path, NULL);

	data->session_path = g_strdup(session_path);

	guint64 window_id = g_ascii_strtoull(
		obs_data_get_string(data->settings, "window-id"), NULL, 0);

	if (window_id == 0) {
		stream_res = g_dbus_connection_call_sync(
			dbus, "org.gnome.Mutter.ScreenCast", session_path,
			"org.gnome.Mutter.ScreenCast.Session", "RecordMonitor",
			g_variant_new_parsed("(%s, {'cursor-mode' : <%u>})",
					     obs_data_get_string(data->settings,
								 "connector"),
					     obs_data_get_bool(data->settings,
							       "cursor")
						     ? 1
						     : 0),
			NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
	} else {
		stream_res = g_dbus_connection_call_sync(
			dbus, "org.gnome.Mutter.ScreenCast", session_path,
			"org.gnome.Mutter.ScreenCast.Session", "RecordWindow",
			g_variant_new_parsed(
				"({'window-id' : <%t>, 'cursor-mode' : <%u>},)",
				window_id,
				obs_data_get_bool(data->settings, "cursor")
					? 1
					: 0),
			NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
	}

	if (err != NULL) {
		blog(LOG_ERROR, "Cannot call %s on DBus: %s",
		     window_id == 0 ? "RecordMonitor()" : "RecordWindow()",
		     err->message);
		g_error_free(err);

		goto fail;
	}

	gchar *stream_path;
	g_variant_get(stream_res, "(o)", &stream_path, NULL);

	data->subscribe_id = g_dbus_connection_signal_subscribe(
		dbus, NULL, NULL, "Closed", session_path, NULL,
		G_DBUS_CALL_FLAGS_NONE, dbus_stream_closed_cb, data, NULL);

	g_dbus_connection_signal_subscribe(dbus, NULL, NULL,
					   "PipeWireStreamAdded", stream_path,
					   NULL, G_DBUS_CALL_FLAGS_NONE,
					   dbus_cb, data, NULL);

	g_dbus_connection_call_sync(dbus, "org.gnome.Mutter.ScreenCast",
				    session_path,
				    "org.gnome.Mutter.ScreenCast.Session",
				    "Start", NULL, NULL, G_DBUS_CALL_FLAGS_NONE,
				    -1, NULL, &err);

	if (err != NULL) {
		blog(LOG_ERROR, "Cannot call Start() on DBus: %s",
		     err->message);
		g_error_free(err);

		goto fail;
	}

fail:
	if (session_res != NULL)
		g_variant_unref(session_res);

	if (stream_res != NULL)
		g_variant_unref(stream_res);

	if (dbus != NULL)
		g_object_unref(dbus);
}

static void *create(obs_data_t *settings, obs_source_t *source)
{
	data_t *data = g_new0(data_t, 1);

	data->source = source;
	data->settings = settings;

	return data;
}

static void stop(data_t *data)
{
	GError *err = NULL;

	if (data->pipe == NULL) {
		return;
	}

	gst_element_set_state(data->pipe, GST_STATE_NULL);

	GstBus *bus = gst_element_get_bus(data->pipe);
	gst_bus_remove_watch(bus);
	gst_object_unref(bus);

	gst_object_unref(data->pipe);
	data->pipe = NULL;

	GDBusConnection *dbus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
	if (err != NULL) {
		blog(LOG_ERROR, "Cannot connect to DBus: %s", err->message);
		g_error_free(err);

		goto fail;
	}

	g_dbus_connection_signal_unsubscribe(dbus, data->subscribe_id);
	data->subscribe_id = 0;

	g_dbus_connection_call_sync(dbus, "org.gnome.Mutter.ScreenCast",
				    data->session_path,
				    "org.gnome.Mutter.ScreenCast.Session",
				    "Stop", NULL, NULL, G_DBUS_CALL_FLAGS_NONE,
				    -1, NULL, &err);

	if (err != NULL) {
		blog(LOG_ERROR, "Cannot call Stop() on DBus: %s", err->message);
		g_error_free(err);

		goto fail;
	}

fail:
	g_free(data->session_path);
	data->session_path = NULL;

	if (dbus != NULL)
		g_object_unref(dbus);

	obs_source_output_video(data->source, NULL);
}

static void destroy(void *data)
{
	stop(data);
	g_free(data);
}

static void get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "connector", "");
	obs_data_set_default_string(settings, "window-id", "");
	obs_data_set_default_bool(settings, "cursor", true);
	obs_data_set_default_bool(settings, "timestamps", false);
}

static obs_properties_t *get_properties(void *p)
{
	data_t *data = (data_t *)p;

	obs_properties_t *props = obs_properties_create();
	obs_property_t *prop = obs_properties_add_list(props, "connector",
						       "Connector",
						       OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);

	update_plug_names(data);
	for (gint i = 0; i < data->num_plugs; i++) {
		gchar *tmp = g_strdup_printf("%s (%s)", data->plugs[i].monitor,
					     data->plugs[i].connector);
		obs_property_list_add_string(prop, tmp,
					     data->plugs[i].connector);
		g_free(tmp);
	}

	obs_properties_add_text(props, "window-id", "Window ID",
				OBS_TEXT_DEFAULT);
	obs_properties_add_bool(props, "cursor", "Draw mouse cursor");
	obs_properties_add_bool(props, "timestamps",
				"Use running time as time stamps");

	return props;
}

static void update(void *data, obs_data_t *settings)
{
	if (((data_t *)data)->session_path == NULL) {
		return;
	}

	stop(data);
	start(data);
}

static void show(void *data)
{
	start(data);
}

static void hide(void *data)
{
	stop(data);
}

bool obs_module_load(void)
{
	struct obs_source_info info = {
		.id = "gnome-mutter-screencast-source",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_ASYNC_VIDEO |
				OBS_SOURCE_DO_NOT_DUPLICATE,

		.get_name = get_name,
		.create = create,
		.destroy = destroy,

		.get_defaults = get_defaults,
		.get_properties = get_properties,
		.update = update,
		.show = show,
		.hide = hide,
	};

	obs_register_source(&info);

	gst_init(NULL, NULL);

	return true;
}
