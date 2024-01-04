#include <glib.h>
#include <glib-unix.h>
#include <gio/gunixinputstream.h>
#include <libsoup/soup.h>
#include <unistd.h>
#include <json-glib/json-glib.h>

static gchar *ws_server_addr = "localhost";
static gint ws_server_port = 8080;
static GMainLoop *main_loop;

static GOptionEntry opt_entries[] = {
    {"address", 'a', 0, G_OPTION_ARG_STRING, &ws_server_addr, "Websocket server address (default: localhost)", NULL},
    {"port", 'p', 0, G_OPTION_ARG_INT, &ws_server_port, "Websocket server port (default: 8080)", NULL},
    {NULL},
};

static gboolean sig_handler(gpointer data)
{
    g_main_loop_quit(main_loop);
    return G_SOURCE_REMOVE;
}

/**
 * Here, we handle "GraXpert's" responses
*/
static void on_message(SoupWebsocketConnection *conn, gint type, GBytes *message, gpointer data)
{
    if (type == SOUP_WEBSOCKET_DATA_TEXT)
    {
        gsize sz;
        const gchar *ptr;

        ptr = g_bytes_get_data(message, &sz);
        g_print("Received text data: %s\n", ptr);

        JsonParser *parser = json_parser_new();
        json_parser_load_from_data(parser, ptr, -1, NULL);
        JsonNode *root;
        root = json_parser_get_root(parser);
        JsonReader *reader = json_reader_new(root);
        json_reader_read_member(reader, "event_type");
        const gchar *event_type = json_reader_get_string_value(reader);
        json_reader_end_member(reader);

        if (strcmp("PROCESS_IMAGE_RESPONSE", event_type) == 0)
        {
            json_reader_read_member(reader, "processing_status");
            const gchar *processing_status = json_reader_get_string_value(reader);
            json_reader_end_member(reader);
            json_reader_read_member(reader, "message");
            const gchar *response_message = json_reader_get_string_value(reader);
            json_reader_end_member(reader);

            g_print("Received PROCESS_IMAGE_RESPONSE. Processing status: %s. Response message: %s.\n", processing_status, response_message);
        }
        else if (strcmp("PARSE_ERROR", event_type) == 0)
        {
            json_reader_read_member(reader, "message");
            const gchar *response_message = json_reader_get_string_value(reader);

            g_print("Received PARSE_ERROR event. Response message: %s.\n", response_message);
        }
        else if (strcmp("UNKNOWN_EVENT_ERROR", event_type) == 0)
        {
            json_reader_read_member(reader, "message");
            const gchar *response_message = json_reader_get_string_value(reader);

            g_print("Received UNKNOWN_EVENT_ERROR event. Response message: %s.\n", response_message);
        }

        g_object_unref(reader);
        g_object_unref(parser);
    }
    else if (type == SOUP_WEBSOCKET_DATA_BINARY)
    {
        g_print("Received binary data (not shown)\n");
    }
    else
    {
        g_print("Invalid data type: %d\n", type);
    }
}

static void on_close(SoupWebsocketConnection *conn, gpointer data)
{
    soup_websocket_connection_close(conn, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
    g_print("WebSocket connection closed\n");
    g_main_loop_quit(main_loop);
}

/**
 * Called when a WebSocket connection has been established.
 * Here, we send an exemplary PROCESS_IMAGE_REQUEST.
*/
static void on_connection(SoupSession *session, GAsyncResult *res, gpointer data)
{
    SoupWebsocketConnection *conn;
    GError *error = NULL;

    conn = soup_session_websocket_connect_finish(session, res, &error);
    if (error)
    {
        g_print("on_connection Error: %s\n", error->message);
        g_error_free(error);
        g_main_loop_quit(main_loop);
        return;
    }

    g_print("Connection successful\n");

    g_signal_connect(conn, "message", G_CALLBACK(on_message), NULL);
    g_signal_connect(conn, "closed", G_CALLBACK(on_close), NULL);

    sleep(1);

    JsonBuilder *builder = json_builder_new();

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "event_type");
    json_builder_add_string_value(builder, "PROCESS_IMAGE_REQUEST");
    json_builder_set_member_name(builder, "filename");
    json_builder_add_string_value(builder, "some_file");
    json_builder_end_object(builder);

    JsonGenerator *gen = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(gen, root);
    gchar *str = json_generator_to_data(gen, NULL);

    g_print("Sending message: %s\n", str);

    soup_websocket_connection_send_text(conn, str);

    g_free(str);
    json_node_free(root);
    g_object_unref(gen);
    g_object_unref(builder);
}

void child_watch_cb(GPid pid, gint status, gpointer user_data)
{
    g_print("GraXpert is being closed\n");
    g_spawn_close_pid(pid);
}

int main(int argc, char **argv)
{
    g_autoptr(GError) error = NULL;

    GOptionContext *context;

    context = g_option_context_new("- WebSocket testing client");
    g_option_context_add_main_entries(context, opt_entries, NULL);
    if (!g_option_context_parse(context, &argc, &argv, &error))
    {
        g_error_free(error);
        return 1;
    }

    g_print("Starting GraXpert...\n");

    // start GraXpert process
    GPid child_pid;
    int retval = -1;

    char *my_argv[64] = {0};
    int nb = 0;
    my_argv[nb++] = "python3";
    my_argv[nb++] = "graxpert_moc.py";
    my_argv[nb++] = NULL;

    g_print("Spawning child process...\n");
    g_spawn_async(NULL, my_argv, NULL,
                  G_SPAWN_SEARCH_PATH |
                      G_SPAWN_LEAVE_DESCRIPTORS_OPEN | G_SPAWN_STDERR_TO_DEV_NULL | G_SPAWN_DO_NOT_REAP_CHILD,
                  NULL, NULL, &child_pid, &error);

    g_child_watch_add(child_pid, child_watch_cb, NULL);

    if (error != NULL)
    {
        g_error("error: %s\n", error->message);
        return retval;
    }

    g_print("GraXpert started\n");

    main_loop = g_main_loop_new(NULL, FALSE);

    g_unix_signal_add(SIGINT, (GSourceFunc)sig_handler, NULL);

    SoupSession *session;
    SoupMessage *msg;

    // Create the soup session
    gchar *uri = NULL;
    session = soup_session_new();

    uri = g_strdup_printf("%s://%s:%d", "ws", ws_server_addr, ws_server_port);
    msg = soup_message_new(SOUP_METHOD_GET, uri);
    g_free(uri);

    sleep(1);

    soup_session_websocket_connect_async(
        session,
        msg,
        NULL, NULL, NULL,
        (GAsyncReadyCallback)on_connection,
        NULL);

    g_print("start main loop\n");
    g_main_loop_run(main_loop);

    g_main_loop_unref(main_loop);

    g_spawn_close_pid(child_pid);

    return 0;
}
