/*
 * GUI (Fase 4) del proyecto de compresor Huffman.
 *
 * Disenio: en vez de reimplementar la orquestacion de fork()/pthread()
 * DENTRO del proceso de la GUI, esta interfaz invoca los 6 binarios ya
 * compilados (compresor/descompresor serial, fork y thread) como
 * subprocesos independientes con g_spawn_sync(), y lee su salida por
 * stdout para obtener las metricas.
 *
 * Esto no es solo por simplicidad: mezclar fork() con un proceso que ya
 * tiene varios hilos corriendo (como GTK, que usa hilos internamente)
 * es un patron riesgoso en C -- si otro hilo tiene tomado el lock
 * interno de malloc() justo en el instante del fork(), el proceso hijo
 * puede quedar con ese lock tomado para siempre y bloquearse en su
 * primer malloc(). Al invocar los binarios via exec() (que es lo que
 * hace g_spawn), el hijo arranca como un proceso nuevo, de un solo
 * hilo, sin arrastrar ese problema.
 *
 * Cada uno de los 6 programas, ademas de su salida "humana" habitual,
 * imprime una ultima linea de la forma:
 *
 *   RESULT ok=1 elapsed=0.001234 original_size=123 compressed_size=45
 *   RESULT ok=1 elapsed=0.000987 verified=13 total=13
 *
 * que esta GUI parsea para obtener los numeros exactos sin tener que
 * re-calcular nada por su cuenta.
 */

#include <gtk/gtk.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

/* ==================== Ubicar los binarios hermanos ==================== */

static gchar *get_self_bin_dir(void) {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return g_strdup(".");
    buf[n] = '\0';
    return g_path_get_dirname(buf);
}

/* ==================== Ejecutar un binario y parsear su RESULT ==================== */

typedef struct {
    gboolean ok;
    double elapsed;
    guint64 original_size;
    guint64 compressed_size;
    int verified;
    int total;
    char *raw_output; /* stdout+stderr combinados, para el log/depuracion */
} ProgramResult;

/* Busca 'key' (con el '=' incluido) dentro de 'line' y copia el valor
 * (hasta el siguiente espacio o fin de linea) en 'buf'. */
static gboolean extract_field(const char *line, const char *key, char *buf, size_t buf_size) {
    const char *p = strstr(line, key);
    if (!p) return FALSE;
    p += strlen(key);
    const char *end = p;
    while (*end && *end != ' ' && *end != '\n') end++;
    size_t len = (size_t)(end - p);
    if (len >= buf_size) len = buf_size - 1;
    memcpy(buf, p, len);
    buf[len] = '\0';
    return TRUE;
}

static void program_result_free(ProgramResult *r) {
    if (!r) return;
    g_free(r->raw_output);
}

/* argv debe terminar en NULL. Ejecuta de forma SINCRONA (bloqueante) --
 * por eso todo esto se corre en un hilo de fondo (ver worker_thread_func),
 * nunca directamente en el hilo principal de GTK. */
static gboolean run_program(char **argv, ProgramResult *out) {
    memset(out, 0, sizeof(*out));

    gchar *out_buf = NULL;
    gchar *err_buf = NULL;
    gint status = 0;
    GError *error = NULL;

    gboolean spawned = g_spawn_sync(
        NULL,           /* directorio de trabajo: heredar el actual */
        argv,
        NULL,           /* envp: heredar el ambiente actual */
        0,
        NULL, NULL,     /* child_setup */
        &out_buf, &err_buf, &status, &error);

    if (!spawned) {
        out->ok = FALSE;
        out->raw_output = g_strdup_printf("(no se pudo iniciar '%s': %s)\n",
                                           argv[0], error ? error->message : "error desconocido");
        g_clear_error(&error);
        g_free(out_buf);
        g_free(err_buf);
        return FALSE;
    }

    out->raw_output = out_buf ? out_buf : g_strdup("");
    if (err_buf && *err_buf) {
        gchar *combined = g_strconcat(out->raw_output, err_buf, NULL);
        g_free(out->raw_output);
        out->raw_output = combined;
    }
    g_free(err_buf);

    const char *result_line = strstr(out->raw_output, "RESULT ok=1");
    if (!result_line) {
        out->ok = FALSE;
        return FALSE;
    }
    out->ok = TRUE;

    char field[64];
    if (extract_field(result_line, "elapsed=", field, sizeof(field)))
        out->elapsed = atof(field);
    if (extract_field(result_line, "original_size=", field, sizeof(field)))
        out->original_size = g_ascii_strtoull(field, NULL, 10);
    if (extract_field(result_line, "compressed_size=", field, sizeof(field)))
        out->compressed_size = g_ascii_strtoull(field, NULL, 10);
    if (extract_field(result_line, "verified=", field, sizeof(field)))
        out->verified = atoi(field);
    if (extract_field(result_line, "total=", field, sizeof(field)))
        out->total = atoi(field);

    return TRUE;
}

/* ==================== Estado de la aplicacion / widgets ==================== */

typedef struct {
    GtkWidget *window;
    GtkWidget *dir_label;
    GtkWidget *recursive_check;
    GtkWidget *fork_spin;
    GtkWidget *thread_spin;
    GtkWidget *run_button;
    GtkWidget *choose_button;
    GtkWidget *spinner;
    GtkWidget *status_label;
    GtkWidget *log_view;
    GtkListStore *list_store;

    gchar *selected_dir;
    gchar *bin_dir;
} AppWidgets;

enum {
    COL_VERSION = 0,
    COL_HEALTH,
    COL_TIME_COMPRESS,
    COL_TIME_DECOMPRESS,
    COL_SPEEDUP_COMPRESS,
    COL_SPEEDUP_DECOMPRESS,
    COL_SIZE_ORIGINAL,
    COL_SIZE_COMPRESSED,
    COL_RATIO,
    N_COLS
};

/* ==================== Resultado completo de correr una version ==================== */

typedef struct {
    ProgramResult compress;
    ProgramResult decompress;
} VersionRun;

typedef struct {
    VersionRun serial, fork, thread;
    gchar *work_dir;      /* donde quedaron los .hzip y las carpetas extraidas */
    gchar *error_message; /* si algo fallo de forma irrecuperable */
} AllResults;

/* ==================== Actualizar la UI desde el hilo principal ==================== */

static gboolean update_status_idle(gpointer data) {
    AppWidgets *app = ((gpointer *)data)[0];
    gchar *msg = ((gpointer *)data)[1];
    gtk_label_set_text(GTK_LABEL(app->status_label), msg);
    g_free(msg);
    g_free(data);
    return G_SOURCE_REMOVE;
}

static void post_status(AppWidgets *app, const gchar *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    gchar *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);

    gpointer *data = g_new(gpointer, 2);
    data[0] = app;
    data[1] = msg;
    g_idle_add(update_status_idle, data);
}

static gchar *format_bytes(guint64 bytes) {
    if (bytes >= 1024 * 1024)
        return g_strdup_printf("%.2f MB", bytes / (1024.0 * 1024.0));
    if (bytes >= 1024)
        return g_strdup_printf("%.2f KB", bytes / 1024.0);
    return g_strdup_printf("%" G_GUINT64_FORMAT " B", bytes);
}

/* Agrega una fila a la tabla comparativa. 'serial_c_elapsed' /
 * 'serial_d_elapsed' son los tiempos de la corrida serial, usados como
 * base para calcular el % de aceleracion (para la fila "Serial" misma,
 * el resultado da 0%, que es lo esperado: es la base de comparacion). */
static void add_result_row(AppWidgets *app, const char *version_name,
                            VersionRun *run, double serial_c_elapsed, double serial_d_elapsed) {
    GtkTreeIter iter;

    if (!run->compress.ok || !run->decompress.ok) {
        gtk_list_store_append(app->list_store, &iter);
        gtk_list_store_set(app->list_store, &iter,
            COL_VERSION, version_name,
            COL_HEALTH, "-", COL_TIME_COMPRESS, "-", COL_TIME_DECOMPRESS, "-",
            COL_SPEEDUP_COMPRESS, "-", COL_SPEEDUP_DECOMPRESS, "-",
            COL_SIZE_ORIGINAL, "-", COL_SIZE_COMPRESSED, "-", COL_RATIO, "ERROR",
            -1);
        return;
    }

    double health = run->decompress.total > 0
        ? (100.0 * run->decompress.verified / run->decompress.total) : 100.0;

    double speedup_c = serial_c_elapsed > 0
        ? (100.0 * (serial_c_elapsed - run->compress.elapsed) / serial_c_elapsed) : 0.0;
    double speedup_d = serial_d_elapsed > 0
        ? (100.0 * (serial_d_elapsed - run->decompress.elapsed) / serial_d_elapsed) : 0.0;

    double ratio = run->compress.original_size > 0
        ? (100.0 * (double)run->compress.compressed_size / (double)run->compress.original_size) : 0.0;

    gchar *health_s = g_strdup_printf("%.2f%% (%d/%d)", health, run->decompress.verified, run->decompress.total);
    gchar *tc_s = g_strdup_printf("%.4f s", run->compress.elapsed);
    gchar *td_s = g_strdup_printf("%.4f s", run->decompress.elapsed);
    gchar *sc_s = g_strdup_printf("%+.1f%%", speedup_c);
    gchar *sd_s = g_strdup_printf("%+.1f%%", speedup_d);
    gchar *orig_s = format_bytes(run->compress.original_size);
    gchar *comp_s = format_bytes(run->compress.compressed_size);
    gchar *ratio_s = g_strdup_printf("%.2f%%", ratio);

    gtk_list_store_append(app->list_store, &iter);
    gtk_list_store_set(app->list_store, &iter,
        COL_VERSION, version_name,
        COL_HEALTH, health_s,
        COL_TIME_COMPRESS, tc_s,
        COL_TIME_DECOMPRESS, td_s,
        COL_SPEEDUP_COMPRESS, sc_s,
        COL_SPEEDUP_DECOMPRESS, sd_s,
        COL_SIZE_ORIGINAL, orig_s,
        COL_SIZE_COMPRESSED, comp_s,
        COL_RATIO, ratio_s,
        -1);

    g_free(health_s); g_free(tc_s); g_free(td_s); g_free(sc_s); g_free(sd_s);
    g_free(orig_s); g_free(comp_s); g_free(ratio_s);
}

typedef struct {
    AppWidgets *app;
    AllResults *results;
} FinishData;

static gboolean finish_run_idle(gpointer data) {
    FinishData *fd = data;
    AppWidgets *app = fd->app;
    AllResults *r = fd->results;

    gtk_list_store_clear(app->list_store);

    if (r->error_message) {
        gtk_label_set_text(GTK_LABEL(app->status_label), r->error_message);
    } else {
        double serial_c = r->serial.compress.ok ? r->serial.compress.elapsed : 0.0;
        double serial_d = r->serial.decompress.ok ? r->serial.decompress.elapsed : 0.0;

        add_result_row(app, "Serial", &r->serial, serial_c, serial_d);
        add_result_row(app, "Fork + IPC (pipes)", &r->fork, serial_c, serial_d);
        add_result_row(app, "Pthreads + mem. compartida", &r->thread, serial_c, serial_d);

        gchar *status = g_strdup_printf(
            "Comparativa completa. Archivos .hzip y carpetas extraidas en: %s", r->work_dir);
        gtk_label_set_text(GTK_LABEL(app->status_label), status);
        g_free(status);
    }

    /* Volcar el log combinado de las 6 corridas al GtkTextView */
    GString *log = g_string_new(NULL);
    g_string_append_printf(log, "=== SERIAL (compresion) ===\n%s\n", r->serial.compress.raw_output ? r->serial.compress.raw_output : "");
    g_string_append_printf(log, "=== SERIAL (descompresion) ===\n%s\n", r->serial.decompress.raw_output ? r->serial.decompress.raw_output : "");
    g_string_append_printf(log, "=== FORK (compresion) ===\n%s\n", r->fork.compress.raw_output ? r->fork.compress.raw_output : "");
    g_string_append_printf(log, "=== FORK (descompresion) ===\n%s\n", r->fork.decompress.raw_output ? r->fork.decompress.raw_output : "");
    g_string_append_printf(log, "=== THREAD (compresion) ===\n%s\n", r->thread.compress.raw_output ? r->thread.compress.raw_output : "");
    g_string_append_printf(log, "=== THREAD (descompresion) ===\n%s\n", r->thread.decompress.raw_output ? r->thread.decompress.raw_output : "");

    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->log_view));
    gtk_text_buffer_set_text(buf, log->str, -1);
    g_string_free(log, TRUE);

    /* Reactivar controles */
    gtk_widget_set_sensitive(app->run_button, TRUE);
    gtk_widget_set_sensitive(app->choose_button, TRUE);
    gtk_spinner_stop(GTK_SPINNER(app->spinner));
    gtk_widget_set_visible(app->spinner, FALSE);

    program_result_free(&r->serial.compress);
    program_result_free(&r->serial.decompress);
    program_result_free(&r->fork.compress);
    program_result_free(&r->fork.decompress);
    program_result_free(&r->thread.compress);
    program_result_free(&r->thread.decompress);
    g_free(r->work_dir);
    g_free(r->error_message);
    g_free(r);
    g_free(fd);

    return G_SOURCE_REMOVE;
}

/* ==================== Hilo de fondo: corre las 6 combinaciones ==================== */

typedef struct {
    AppWidgets *app;
    gchar *dir_path;
    gboolean recursive;
    int fork_workers;
    int pthread_threads;
} RunParams;

static void run_compress(AppWidgets *app, const char *bin_name, const char *dir_path,
                          const char *out_path, gboolean recursive, int workers /* -1 = sin arg */,
                          ProgramResult *out) {
    gchar *bin_path = g_build_filename(app->bin_dir, bin_name, NULL);
    GPtrArray *args = g_ptr_array_new();
    g_ptr_array_add(args, bin_path);
    g_ptr_array_add(args, (gpointer)dir_path);
    g_ptr_array_add(args, (gpointer)out_path);

    gchar *workers_str = NULL;
    if (workers >= 0) {
        /* Necesitamos un placeholder en la posicion de --recursivo para
         * que el argumento de workers/hilos caiga en la posicion
         * correcta (ver como parsean argv los programas fork/thread). */
        g_ptr_array_add(args, recursive ? (gpointer)"--recursivo" : (gpointer)"");
        workers_str = g_strdup_printf("%d", workers);
        g_ptr_array_add(args, workers_str);
    } else if (recursive) {
        g_ptr_array_add(args, (gpointer)"--recursivo");
    }
    g_ptr_array_add(args, NULL);

    run_program((char **)args->pdata, out);

    g_free(workers_str);
    g_ptr_array_free(args, TRUE);
    g_free(bin_path);
}

static void run_decompress(AppWidgets *app, const char *bin_name, const char *archive_path,
                            const char *out_dir, int workers /* -1 = sin arg */, ProgramResult *out) {
    gchar *bin_path = g_build_filename(app->bin_dir, bin_name, NULL);
    GPtrArray *args = g_ptr_array_new();
    g_ptr_array_add(args, bin_path);
    g_ptr_array_add(args, (gpointer)archive_path);
    g_ptr_array_add(args, (gpointer)out_dir);

    gchar *workers_str = NULL;
    if (workers >= 0) {
        workers_str = g_strdup_printf("%d", workers);
        g_ptr_array_add(args, workers_str);
    }
    g_ptr_array_add(args, NULL);

    run_program((char **)args->pdata, out);

    g_free(workers_str);
    g_ptr_array_free(args, TRUE);
    g_free(bin_path);
}

static gpointer worker_thread_func(gpointer data) {
    RunParams *p = data;
    AppWidgets *app = p->app;

    AllResults *results = g_new0(AllResults, 1);

    GError *error = NULL;
    gchar *work_dir = g_dir_make_tmp("hzip_gui_XXXXXX", &error);
    if (!work_dir) {
        results->error_message = g_strdup_printf("No se pudo crear directorio temporal: %s",
                                                   error ? error->message : "?");
        g_clear_error(&error);
        goto done;
    }
    results->work_dir = work_dir;

    gchar *serial_hzip = g_build_filename(work_dir, "serial.hzip", NULL);
    gchar *fork_hzip   = g_build_filename(work_dir, "fork.hzip", NULL);
    gchar *thread_hzip = g_build_filename(work_dir, "thread.hzip", NULL);
    gchar *serial_out  = g_build_filename(work_dir, "salida_serial", NULL);
    gchar *fork_out    = g_build_filename(work_dir, "salida_fork", NULL);
    gchar *thread_out  = g_build_filename(work_dir, "salida_thread", NULL);

    post_status(app, "Comprimiendo con la version serial...");
    run_compress(app, "compresor_serial", p->dir_path, serial_hzip, p->recursive, -1,
                 &results->serial.compress);

    post_status(app, "Comprimiendo con fork() + pipes (%d procesos)...", p->fork_workers);
    run_compress(app, "compressor_parallel", p->dir_path, fork_hzip, p->recursive, p->fork_workers,
                 &results->fork.compress);

    post_status(app, "Comprimiendo con pthreads (%d hilos)...", p->pthread_threads);
    run_compress(app, "compressor_thread", p->dir_path, thread_hzip, p->recursive, p->pthread_threads,
                 &results->thread.compress);

    post_status(app, "Descomprimiendo (serial)...");
    run_decompress(app, "descompresor_serial", serial_hzip, serial_out, -1,
                   &results->serial.decompress);

    post_status(app, "Descomprimiendo (fork + pipes)...");
    run_decompress(app, "decompressor_parallel", fork_hzip, fork_out, p->fork_workers,
                   &results->fork.decompress);

    post_status(app, "Descomprimiendo (pthreads)...");
    run_decompress(app, "decompressor_thread", thread_hzip, thread_out, p->pthread_threads,
                   &results->thread.decompress);

    g_free(serial_hzip); g_free(fork_hzip); g_free(thread_hzip);
    g_free(serial_out); g_free(fork_out); g_free(thread_out);

done:
    {
        FinishData *fd = g_new(FinishData, 1);
        fd->app = app;
        fd->results = results;
        g_idle_add(finish_run_idle, fd);
    }

    g_free(p->dir_path);
    g_free(p);
    return NULL;
}

/* ==================== Callbacks de la UI ==================== */

static void on_run_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppWidgets *app = user_data;

    if (!app->selected_dir) {
        gtk_label_set_text(GTK_LABEL(app->status_label), "Primero elegi un directorio.");
        return;
    }

    gtk_widget_set_sensitive(app->run_button, FALSE);
    gtk_widget_set_sensitive(app->choose_button, FALSE);
    gtk_widget_set_visible(app->spinner, TRUE);
    gtk_spinner_start(GTK_SPINNER(app->spinner));
    gtk_label_set_text(GTK_LABEL(app->status_label), "Iniciando...");

    RunParams *p = g_new0(RunParams, 1);
    p->app = app;
    p->dir_path = g_strdup(app->selected_dir);
    p->recursive = gtk_check_button_get_active(GTK_CHECK_BUTTON(app->recursive_check));
    p->fork_workers = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(app->fork_spin));
    p->pthread_threads = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(app->thread_spin));

    GThread *thread = g_thread_new("hzip-run", worker_thread_func, p);
    g_thread_unref(thread);
}

static void on_folder_chosen(GObject *source, GAsyncResult *res, gpointer user_data) {
    AppWidgets *app = user_data;
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    GError *error = NULL;

    GFile *folder = gtk_file_dialog_select_folder_finish(dialog, res, &error);
    if (folder) {
        g_free(app->selected_dir);
        app->selected_dir = g_file_get_path(folder);
        gtk_label_set_text(GTK_LABEL(app->dir_label), app->selected_dir);
        gtk_widget_set_sensitive(app->run_button, TRUE);
        g_object_unref(folder);
    } else if (error) {
        g_clear_error(&error);
    }
}

static void on_choose_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppWidgets *app = user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Elegir directorio a comprimir");
    gtk_file_dialog_select_folder(dialog, GTK_WINDOW(app->window), NULL, on_folder_chosen, app);
    g_object_unref(dialog);
}

/* ==================== Construccion de la ventana ==================== */

static GtkWidget *make_labeled_row(const char *label_text, GtkWidget *control) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new(label_text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_size_request(label, 180, -1);
    gtk_box_append(GTK_BOX(row), label);
    gtk_box_append(GTK_BOX(row), control);
    return row;
}

static void activate(GtkApplication *gtk_app, gpointer user_data) {
    (void)user_data;
    AppWidgets *app = g_new0(AppWidgets, 1);
    app->bin_dir = get_self_bin_dir();

    app->window = gtk_application_window_new(gtk_app);
    gtk_window_set_title(GTK_WINDOW(app->window), "Comparador de Compresores Huffman - Proyecto SO");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 900, 600);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(root, 16);
    gtk_widget_set_margin_end(root, 16);
    gtk_widget_set_margin_top(root, 16);
    gtk_widget_set_margin_bottom(root, 16);
    gtk_window_set_child(GTK_WINDOW(app->window), root);

    /* --- Seleccion de directorio --- */
    GtkWidget *dir_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app->choose_button = gtk_button_new_with_label("Elegir directorio...");
    app->dir_label = gtk_label_new("(ningun directorio seleccionado)");
    gtk_widget_set_halign(app->dir_label, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(app->dir_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(app->dir_label, TRUE);
    gtk_box_append(GTK_BOX(dir_row), app->choose_button);
    gtk_box_append(GTK_BOX(dir_row), app->dir_label);
    gtk_box_append(GTK_BOX(root), dir_row);

    /* --- Opciones --- */
    app->recursive_check = gtk_check_button_new_with_label("Incluir subdirectorios (--recursivo)");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(app->recursive_check), TRUE);
    gtk_box_append(GTK_BOX(root), app->recursive_check);

    app->fork_spin = gtk_spin_button_new_with_range(1, 32, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->fork_spin), 4);
    gtk_box_append(GTK_BOX(root), make_labeled_row("Procesos (fork):", app->fork_spin));

    app->thread_spin = gtk_spin_button_new_with_range(1, 32, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->thread_spin), 4);
    gtk_box_append(GTK_BOX(root), make_labeled_row("Hilos (pthreads):", app->thread_spin));

    /* --- Boton de ejecucion + spinner --- */
    GtkWidget *run_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app->run_button = gtk_button_new_with_label("Comprimir y descomprimir con las 3 versiones");
    gtk_widget_add_css_class(app->run_button, "suggested-action");
    gtk_widget_set_sensitive(app->run_button, FALSE);
    app->spinner = gtk_spinner_new();
    gtk_widget_set_visible(app->spinner, FALSE);
    gtk_box_append(GTK_BOX(run_row), app->run_button);
    gtk_box_append(GTK_BOX(run_row), app->spinner);
    gtk_box_append(GTK_BOX(root), run_row);

    app->status_label = gtk_label_new("Elegi un directorio para empezar.");
    gtk_widget_set_halign(app->status_label, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(app->status_label), TRUE);
    gtk_box_append(GTK_BOX(root), app->status_label);

    /* --- Tabla comparativa --- */
    app->list_store = gtk_list_store_new(N_COLS,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

    GtkWidget *tree_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->list_store));
    g_object_unref(app->list_store);

    const char *headers[N_COLS] = {
        "Version", "Salud compresion", "T. compresor", "T. descompresor",
        "Acel. compresor", "Acel. descompresor", "Tam. original", "Tam. comprimido", "Radio compresion"
    };
    for (int i = 0; i < N_COLS; i++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
            headers[i], renderer, "text", i, NULL);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), col);
    }

    GtkWidget *tree_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tree_scroll), tree_view);
    gtk_widget_set_size_request(tree_scroll, -1, 160);
    gtk_box_append(GTK_BOX(root), tree_scroll);

    /* --- Log detallado (colapsable) --- */
    GtkWidget *expander = gtk_expander_new("Ver salida detallada de las 6 corridas");
    app->log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(app->log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(app->log_view), TRUE);
    GtkWidget *log_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(log_scroll), app->log_view);
    gtk_widget_set_size_request(log_scroll, -1, 200);
    gtk_expander_set_child(GTK_EXPANDER(expander), log_scroll);
    gtk_box_append(GTK_BOX(root), expander);

    g_signal_connect(app->choose_button, "clicked", G_CALLBACK(on_choose_clicked), app);
    g_signal_connect(app->run_button, "clicked", G_CALLBACK(on_run_clicked), app);

    gtk_window_present(GTK_WINDOW(app->window));
}

int main(int argc, char **argv) {
    GtkApplication *gtk_app = gtk_application_new(
        "edu.so.compresorhuffman.gui", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gtk_app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(gtk_app), argc, argv);
    g_object_unref(gtk_app);
    return status;
}
