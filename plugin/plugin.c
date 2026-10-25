/* LeiOS boot splash plugin for Plymouth
 *
 * Draws the LeiOS wallpaper with the logo at the top and, below it, a rounded
 * translucent box over most of the screen with the boot messages: the console
 * output Plymouth collects (kernel messages at the console log level and the
 * status lines systemd prints while Plymouth runs), i.e. what a boot without
 * the splash shows, with long lines wrapped like on the console. Password and
 * question prompts get a box of their own below it.
 *
 * Plymouth's text labels paint rich text (the colours of the console) on a
 * black background, made for its full screen console. To put the lines on
 * the translucent box instead, each line is drawn into a buffer of its own on
 * black and black is then made transparent (see render_log_line ()).
 *
 * The theme file (leios.plymouth) configures it in its [leios] group; see
 * create_plugin () for the keys and their defaults.
 *
 * Copyright 2026 Linus Fischer <leicraft@leicraftmc.de>
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Structure and the Plymouth API usage follow Plymouth's two-step plugin
 * (GPL-2.0-or-later).
 */
#define _GNU_SOURCE
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ply-boot-splash-plugin.h"
#include "ply-buffer.h"
#include "ply-console-viewer.h"
#include "ply-event-loop.h"
#include "ply-image.h"
#include "ply-key-file.h"
#include "ply-label.h"
#include "ply-list.h"
#include "ply-logger.h"
#include "ply-pixel-buffer.h"
#include "ply-pixel-display.h"
#include "ply-rich-text.h"
#include "ply-terminal-emulator.h"
#include "ply-trigger.h"
#include "ply-utils.h"

#define UPDATE_INTERVAL (1.0 / 30)
/* Lines and columns the terminal emulator keeps; each view wraps lines to the
 * width of its box and shows as many of the last ones as fit. */
#define TERMINAL_ROWS 256
#define TERMINAL_COLUMNS 256
#define MAX_BULLETS 64
/* Font sizes in the theme file are for a 1080 pixel high screen. */
#define REFERENCE_HEIGHT 1080.0

typedef enum
{
        PLY_BOOT_SPLASH_DISPLAY_NORMAL,
        PLY_BOOT_SPLASH_DISPLAY_QUESTION_ENTRY,
        PLY_BOOT_SPLASH_DISPLAY_PASSWORD_ENTRY
} ply_boot_splash_display_type_t;

typedef struct
{
        ply_boot_splash_plugin_t *plugin;
        ply_pixel_display_t      *display;

        ply_pixel_buffer_t       *background;
        long                      background_x;
        long                      background_y;

        ply_image_t              *logo;
        ply_rectangle_t           logo_area;

        /* Box with the boot messages: the full height, and the shorter one
         * shown above a prompt box. Each of its rows is a label drawn into a
         * buffer of its own. */
        ply_pixel_buffer_t       *log_box;
        ply_pixel_buffer_t       *log_box_short;
        ply_rectangle_t           log_box_area;
        long                      log_box_short_height;
        size_t                    rows;
        size_t                    rows_short;
        ply_label_t             **log_labels;
        ply_pixel_buffer_t      **log_line_buffers;
        long                      text_width;
        size_t                    line_characters;

        /* Box with a password or question prompt */
        ply_pixel_buffer_t       *prompt_box;
        ply_rectangle_t           prompt_box_area;
        ply_label_t              *prompt_label;
        ply_label_t              *entry_label;

        /* Progress of offline updates, below the logo */
        ply_label_t              *status_label;

        char                     *font;
        char                     *monospace_font;
        long                      line_height;
        long                      padding;
        long                      gap;
} view_t;

struct _ply_boot_splash_plugin
{
        ply_event_loop_t              *loop;
        ply_boot_splash_mode_t         mode;
        ply_list_t                    *views;
        ply_boot_splash_display_type_t state;

        char                          *image_dir;
        ply_image_t                   *background_image;
        ply_image_t                   *logo_image;

        /* Settings from the theme file */
        double                         logo_size;
        double                         logo_position;
        double                         box_width;
        double                         box_bottom;
        uint32_t                       box_color;
        double                         box_opacity;
        char                          *font;
        char                          *monospace_font;
        double                         font_size;
        uint32_t                       text_color;

        ply_terminal_emulator_t       *terminal;
        uint32_t                       show_log : 1;
        uint32_t                       log_has_text : 1;
        uint32_t                       update_queued : 1;
        uint32_t                       is_visible : 1;

        char                          *prompt;
        char                          *entry_text;
        int                            bullets;
        char                          *status;
};

ply_boot_splash_plugin_interface_t *ply_boot_splash_plugin_get_interface (void);

static void on_draw (void                *user_data,
                     ply_pixel_buffer_t  *pixel_buffer,
                     int                  x,
                     int                  y,
                     int                  width,
                     int                  height,
                     ply_pixel_display_t *display);

/* ---------- Helpers ---------- */

/* asprintf () that returns the string, or NULL if it fails */
static char *
format_string (const char *format,
               ...)
{
        va_list arguments;
        char *string;
        int result;

        va_start (arguments, format);
        result = vasprintf (&string, format, arguments);
        va_end (arguments);

        return result < 0 ? NULL : string;
}

static char *
key_file_get_string (ply_key_file_t *key_file,
                     const char     *key,
                     const char     *default_value)
{
        char *value;

        value = ply_key_file_get_value (key_file, "leios", key);
        if (value == NULL || value[0] == '\0') {
                free (value);
                return strdup (default_value);
        }

        return value;
}

static uint32_t
key_file_get_color (ply_key_file_t *key_file,
                    const char     *key,
                    uint32_t        default_value)
{
        char *value;
        uint32_t color = default_value;

        value = ply_key_file_get_value (key_file, "leios", key);
        if (value != NULL && value[0] != '\0')
                color = (uint32_t) strtoul (value, NULL, 0);
        free (value);

        return color;
}

static ply_image_t *
load_image (const char *image_dir,
            const char *name)
{
        ply_image_t *image;
        char *path;

        path = format_string ("%s/%s", image_dir, name);
        if (path == NULL)
                return NULL;

        image = ply_image_new (path);
        free (path);

        if (!ply_image_load (image)) {
                ply_trace ("could not load %s", name);
                ply_image_free (image);
                return NULL;
        }

        return image;
}

/* A rounded rectangle in premultiplied ARGB, with anti-aliased corners. */
static ply_pixel_buffer_t *
create_rounded_box (unsigned long width,
                    unsigned long height,
                    double        radius,
                    uint32_t      rgb,
                    double        opacity)
{
        ply_pixel_buffer_t *buffer;
        uint32_t *data;

        buffer = ply_pixel_buffer_new (width, height);
        data = ply_pixel_buffer_get_argb32_data (buffer);

        for (unsigned long y = 0; y < height; y++) {
                for (unsigned long x = 0; x < width; x++) {
                        double px = x + 0.5, py = y + 0.5;
                        double dx = 0.0, dy = 0.0, coverage = 1.0;
                        uint32_t alpha, red, green, blue;

                        if (px < radius)
                                dx = radius - px;
                        else if (px > width - radius)
                                dx = px - (width - radius);
                        if (py < radius)
                                dy = radius - py;
                        else if (py > height - radius)
                                dy = py - (height - radius);

                        if (dx > 0.0 && dy > 0.0) {
                                coverage = radius - sqrt (dx * dx + dy * dy) + 0.5;
                                coverage = CLAMP (coverage, 0.0, 1.0);
                        }

                        alpha = (uint32_t) lround (opacity * coverage * 255.0);
                        red = ((rgb >> 16) & 0xff) * alpha / 255;
                        green = ((rgb >> 8) & 0xff) * alpha / 255;
                        blue = (rgb & 0xff) * alpha / 255;
                        data[y * width + x] = alpha << 24 | red << 16 | green << 8 | blue;
                }
        }

        ply_pixel_buffer_set_opaque (buffer, false);
        return buffer;
}

/* Index of the last line with text, -1 if there is none. The emulator's last
 * line is the one being written, which is empty after a newline. */
static int
get_last_log_line (ply_boot_splash_plugin_t *plugin)
{
        int last;

        last = ply_terminal_emulator_get_line_count (plugin->terminal) - 1;
        if (last >= 0 &&
            ply_rich_text_get_length (ply_terminal_emulator_get_nth_line (plugin->terminal, last)) == 0)
                last--;

        return last;
}

/* ---------- Views ---------- */

static view_t *
view_new (ply_boot_splash_plugin_t *plugin,
          ply_pixel_display_t      *display)
{
        view_t *view;

        view = calloc (1, sizeof(view_t));
        view->plugin = plugin;
        view->display = display;

        view->prompt_label = ply_label_new ();
        view->entry_label = ply_label_new ();
        view->status_label = ply_label_new ();

        return view;
}

/* Frees what view_load () created */
static void
view_unload (view_t *view)
{
        for (size_t i = 0; i < view->rows; i++) {
                ply_label_free (view->log_labels[i]);
                ply_pixel_buffer_free (view->log_line_buffers[i]);
        }
        free (view->log_labels);
        free (view->log_line_buffers);
        view->log_labels = NULL;
        view->log_line_buffers = NULL;
        view->rows = 0;

        if (view->background != NULL)
                ply_pixel_buffer_free (view->background);
        if (view->logo != NULL)
                ply_image_free (view->logo);
        if (view->log_box != NULL)
                ply_pixel_buffer_free (view->log_box);
        if (view->log_box_short != NULL)
                ply_pixel_buffer_free (view->log_box_short);
        if (view->prompt_box != NULL)
                ply_pixel_buffer_free (view->prompt_box);
        view->background = NULL;
        view->logo = NULL;
        view->log_box = NULL;
        view->log_box_short = NULL;
        view->prompt_box = NULL;

        free (view->font);
        free (view->monospace_font);
        view->font = NULL;
        view->monospace_font = NULL;
}

static void
view_free (view_t *view)
{
        view_unload (view);

        ply_label_free (view->prompt_label);
        ply_label_free (view->entry_label);
        ply_label_free (view->status_label);

        free (view);
}

/* Rows of messages shown now: fewer while a prompt box is below them */
static size_t
view_get_visible_rows (view_t *view)
{
        if (view->plugin->state != PLY_BOOT_SPLASH_DISPLAY_NORMAL)
                return view->rows_short;

        return view->rows;
}

static void
view_redraw (view_t *view)
{
        ply_pixel_display_draw_area (view->display, 0, 0,
                                     ply_pixel_display_get_width (view->display),
                                     ply_pixel_display_get_height (view->display));
}

static void
view_redraw_area (view_t          *view,
                  ply_rectangle_t *area)
{
        if (area->width == 0 || area->height == 0)
                return;

        ply_pixel_display_draw_area (view->display, area->x, area->y,
                                     area->width, area->height);
}

/* Draw a line label into its buffer on black and turn the black into
 * transparency: text drawn over black is the text colour premultiplied with
 * its coverage, so the brightest channel serves as the alpha. */
static void
render_log_line (view_t *view,
                 size_t  index)
{
        ply_pixel_buffer_t *buffer = view->log_line_buffers[index];
        unsigned long width = ply_pixel_buffer_get_width (buffer);
        unsigned long height = ply_pixel_buffer_get_height (buffer);
        uint32_t *data = ply_pixel_buffer_get_argb32_data (buffer);

        memset (data, 0, width * height * sizeof(uint32_t));
        ply_label_draw_area (view->log_labels[index], buffer, 0, 0, width, height);

        for (unsigned long i = 0; i < width * height; i++) {
                uint32_t red = (data[i] >> 16) & 0xff;
                uint32_t green = (data[i] >> 8) & 0xff;
                uint32_t blue = data[i] & 0xff;
                uint32_t alpha = MAX (red, MAX (green, blue));

                data[i] = alpha << 24 | red << 16 | green << 8 | blue;
        }
}

/* Rows a message line takes in the box: long lines wrap like on the console */
static size_t
get_line_rows (ply_rich_text_t *line,
               size_t           line_characters)
{
        size_t length = ply_rich_text_get_length (line);

        return length == 0 ? 1 : (length + line_characters - 1) / line_characters;
}

/* Fills the visible rows from the top with the last messages, like a
 * terminal; the rows below the last message stay empty. */
static void
view_update_log (view_t *view)
{
        ply_boot_splash_plugin_t *plugin = view->plugin;
        size_t visible = view_get_visible_rows (view);
        size_t rows_needed = 0, skip_rows = 0, row = 0;
        int first, last, oldest;

        /* Walk back from the last message until the visible rows are full,
         * at most as far back as the emulator keeps lines. */
        last = get_last_log_line (plugin);
        oldest = MAX (0, last - (TERMINAL_ROWS - 2));
        first = last + 1;
        while (first > oldest && rows_needed < visible) {
                first--;
                rows_needed += get_line_rows (ply_terminal_emulator_get_nth_line (plugin->terminal, first),
                                              view->line_characters);
        }
        /* The top line may not fit completely: show only its end */
        if (rows_needed > visible)
                skip_rows = rows_needed - visible;

        for (int line_number = first; line_number <= last && row < visible; line_number++) {
                ply_rich_text_t *line = ply_terminal_emulator_get_nth_line (plugin->terminal, line_number);
                size_t length = ply_rich_text_get_length (line);
                size_t line_rows = get_line_rows (line, view->line_characters);

                ply_rich_text_take_reference (line);
                for (size_t part = line_number == first ? skip_rows : 0; part < line_rows && row < visible; part++) {
                        ply_rich_text_span_t span;

                        if (length == 0) {
                                ply_label_set_text (view->log_labels[row], "");
                        } else {
                                span.offset = part * view->line_characters;
                                span.range = MIN (view->line_characters, length - span.offset);
                                ply_label_set_rich_text (view->log_labels[row], line, &span);
                        }
                        render_log_line (view, row);
                        row++;
                }
                ply_rich_text_drop_reference (line);
        }

        for (; row < view->rows; row++) {
                ply_label_set_text (view->log_labels[row], "");
                render_log_line (view, row);
        }
}

/* The prompt box goes below the (then shorter) message box, or in its place
 * while there are no messages. */
static void
view_place_prompt (view_t *view)
{
        ply_boot_splash_plugin_t *plugin = view->plugin;
        long x, y;

        view->prompt_box_area.x = view->log_box_area.x;
        view->prompt_box_area.y = view->log_box_area.y;
        if (plugin->show_log && plugin->log_has_text)
                view->prompt_box_area.y += view->log_box_short_height + view->gap / 2;

        x = view->prompt_box_area.x + view->padding;
        y = view->prompt_box_area.y + view->padding;
        ply_label_show (view->prompt_label, view->display, x, y);
        ply_label_show (view->entry_label, view->display, x, y + view->line_height * 3 / 2);
}

static void
view_show_status (view_t *view)
{
        ply_boot_splash_plugin_t *plugin = view->plugin;
        long width, x, y;

        if (plugin->status == NULL) {
                ply_label_hide (view->status_label);
                return;
        }

        ply_label_set_text (view->status_label, plugin->status);
        width = ply_label_get_width (view->status_label);
        x = (ply_pixel_display_get_width (view->display) - width) / 2;
        y = view->logo_area.y + view->logo_area.height + (view->gap - view->line_height) / 2;
        ply_label_show (view->status_label, view->display, x, y);
}

static bool
view_load (view_t *view)
{
        ply_boot_splash_plugin_t *plugin = view->plugin;
        unsigned long screen_width, screen_height;
        double scale, font_size;
        long logo_size, box_width, box_height, box_bottom, prompt_height, radius;
        long character_width = 0;
        ply_label_t *measure;

        view_unload (view);

        screen_width = ply_pixel_display_get_width (view->display);
        screen_height = ply_pixel_display_get_height (view->display);
        scale = screen_height / REFERENCE_HEIGHT;

        /* Wallpaper, scaled to cover the screen */
        if (plugin->background_image != NULL) {
                long image_width = ply_image_get_width (plugin->background_image);
                long image_height = ply_image_get_height (plugin->background_image);
                double cover = MAX ((double) screen_width / image_width,
                                    (double) screen_height / image_height);
                long width = (long) ceil (image_width * cover);
                long height = (long) ceil (image_height * cover);
                ply_image_t *scaled;

                uint32_t *data;

                scaled = ply_image_resize (plugin->background_image, width, height);
                view->background = ply_image_convert_to_pixel_buffer (scaled);
                /* Resizing can round the alpha of opaque pixels down, and
                 * Plymouth blends onto pixels that are not fully opaque
                 * differently (wrongly for our purposes). */
                data = ply_pixel_buffer_get_argb32_data (view->background);
                for (long i = 0; i < width * height; i++)
                        data[i] |= 0xff000000;
                ply_pixel_buffer_set_opaque (view->background, true);
                view->background_x = ((long) screen_width - width) / 2;
                view->background_y = ((long) screen_height - height) / 2;
        }

        /* Logo */
        logo_size = MAX (16, lround (screen_height * plugin->logo_size));
        view->logo_area.width = logo_size;
        view->logo_area.height = logo_size;
        view->logo_area.x = ((long) screen_width - logo_size) / 2;
        view->logo_area.y = lround (screen_height * plugin->logo_position) - logo_size / 2;
        if (plugin->logo_image != NULL)
                view->logo = ply_image_resize (plugin->logo_image, logo_size, logo_size);

        /* Fonts and text metrics */
        font_size = CLAMP (plugin->font_size * scale, 9.0, 48.0);
        view->font = format_string ("%s %d", plugin->font, (int) lround (font_size));
        view->monospace_font = format_string ("%s %d", plugin->monospace_font, (int) lround (font_size));
        if (view->font == NULL || view->monospace_font == NULL)
                return false;

        measure = ply_label_new ();
        ply_label_set_font (measure, view->monospace_font);
        ply_label_set_text (measure, "0");
        character_width = ply_label_get_width (measure);
        view->line_height = ply_label_get_height (measure);
        ply_label_free (measure);

        if (character_width <= 1 || view->line_height <= 1) {
                /* No label plugin: nothing can show text */
                character_width = 8;
                view->line_height = 16;
        }

        view->padding = view->line_height * 3 / 4;
        view->gap = lround (screen_height * 0.035);
        radius = view->line_height;

        /* Message box: from below the logo down to MessageBoxBottom, with as
         * many whole rows as fit there */
        box_width = MIN (lround (screen_width * plugin->box_width), (long) screen_width - 2 * view->gap);
        box_bottom = lround (screen_height * plugin->box_bottom);
        prompt_height = view->line_height * 5 / 2 + 2 * view->padding;
        view->text_width = box_width - 2 * view->padding;
        view->line_characters = MAX (1, view->text_width / character_width);
        view->log_box_area.x = ((long) screen_width - box_width) / 2;
        view->log_box_area.y = view->logo_area.y + view->logo_area.height + view->gap;
        view->rows = (size_t) CLAMP ((box_bottom - view->log_box_area.y - 2 * view->padding) / view->line_height,
                                     1, TERMINAL_ROWS - 2);
        box_height = (long) view->rows * view->line_height + 2 * view->padding;
        view->log_box_area.width = box_width;
        view->log_box_area.height = box_height;
        view->log_box = create_rounded_box (box_width, box_height, radius,
                                            plugin->box_color, plugin->box_opacity);

        /* The shorter box above a prompt box, which then ends about where the
         * full message box does */
        view->rows_short = (size_t) CLAMP ((box_height - prompt_height - view->gap / 2 - 2 * view->padding) /
                                           view->line_height,
                                           1, (long) view->rows);
        view->log_box_short_height = (long) view->rows_short * view->line_height + 2 * view->padding;
        view->log_box_short = create_rounded_box (box_width, view->log_box_short_height, radius,
                                                  plugin->box_color, plugin->box_opacity);

        /* The labels are only drawn into their row buffers, at (0, 0) */
        view->log_labels = calloc (view->rows, sizeof(ply_label_t *));
        view->log_line_buffers = calloc (view->rows, sizeof(ply_pixel_buffer_t *));
        for (size_t i = 0; i < view->rows; i++) {
                view->log_labels[i] = ply_label_new ();
                view->log_line_buffers[i] = ply_pixel_buffer_new (view->text_width, view->line_height);
                ply_pixel_buffer_set_opaque (view->log_line_buffers[i], false);
                ply_label_set_font (view->log_labels[i], view->monospace_font);
                ply_label_set_hex_color (view->log_labels[i], plugin->text_color);
                ply_label_show (view->log_labels[i], view->display, 0, 0);
        }

        /* Prompt box: the prompt and the entry below it */
        view->prompt_box_area.width = box_width;
        view->prompt_box_area.height = prompt_height;
        view->prompt_box = create_rounded_box (box_width, view->prompt_box_area.height, radius,
                                               plugin->box_color, plugin->box_opacity);
        ply_label_set_font (view->prompt_label, view->font);
        ply_label_set_hex_color (view->prompt_label, plugin->text_color);
        ply_label_set_font (view->entry_label, view->monospace_font);
        ply_label_set_hex_color (view->entry_label, plugin->text_color);

        ply_label_set_font (view->status_label, view->font);
        ply_label_set_hex_color (view->status_label, plugin->text_color);

        if (plugin->show_log)
                view_update_log (view);
        view_show_status (view);

        return true;
}

static void
load_views (ply_boot_splash_plugin_t *plugin)
{
        ply_list_node_t *node;

        node = ply_list_get_first_node (plugin->views);
        while (node != NULL) {
                ply_list_node_t *next_node = ply_list_get_next_node (plugin->views, node);
                view_t *view = ply_list_node_get_data (node);

                if (!view_load (view)) {
                        ply_trace ("could not load view");
                        view_free (view);
                        ply_list_remove_node (plugin->views, node);
                }

                node = next_node;
        }
}

static void
free_views (ply_boot_splash_plugin_t *plugin)
{
        ply_list_node_t *node;

        node = ply_list_get_first_node (plugin->views);
        while (node != NULL) {
                ply_list_node_t *next_node = ply_list_get_next_node (plugin->views, node);
                view_t *view = ply_list_node_get_data (node);

                ply_pixel_display_set_draw_handler (view->display, NULL, NULL);
                view_free (view);
                ply_list_remove_node (plugin->views, node);

                node = next_node;
        }
}

static void
redraw_views (ply_boot_splash_plugin_t *plugin)
{
        ply_list_node_t *node;

        if (!plugin->is_visible)
                return;

        ply_list_foreach (plugin->views, node) {
                view_redraw (ply_list_node_get_data (node));
        }
}

/* ---------- Drawing ---------- */

static void
on_draw (void                *user_data,
         ply_pixel_buffer_t  *pixel_buffer,
         int                  x,
         int                  y,
         int                  width,
         int                  height,
         ply_pixel_display_t *display)
{
        view_t *view = user_data;
        ply_boot_splash_plugin_t *plugin = view->plugin;
        ply_rectangle_t area = { .x = x, .y = y, .width = width, .height = height };

        if (view->background != NULL)
                ply_pixel_buffer_fill_with_buffer (pixel_buffer, view->background,
                                                   view->background_x, view->background_y);
        else
                ply_pixel_buffer_fill_with_hex_color (pixel_buffer, &area, plugin->box_color);

        if (view->logo != NULL)
                ply_pixel_buffer_fill_with_argb32_data (pixel_buffer, &view->logo_area,
                                                        ply_image_get_data (view->logo));

        if (plugin->status != NULL)
                ply_label_draw_area (view->status_label, pixel_buffer, x, y, width, height);

        if (plugin->show_log && plugin->log_has_text && view->log_box != NULL) {
                bool short_box = plugin->state != PLY_BOOT_SPLASH_DISPLAY_NORMAL;

                ply_pixel_buffer_fill_with_buffer (pixel_buffer, short_box ? view->log_box_short : view->log_box,
                                                   view->log_box_area.x, view->log_box_area.y);
                for (size_t i = 0; i < view_get_visible_rows (view); i++)
                        ply_pixel_buffer_fill_with_buffer (pixel_buffer, view->log_line_buffers[i],
                                                           view->log_box_area.x + view->padding,
                                                           view->log_box_area.y + view->padding +
                                                           (long) i * view->line_height);
        }

        if (plugin->state != PLY_BOOT_SPLASH_DISPLAY_NORMAL && view->prompt_box != NULL) {
                ply_pixel_buffer_fill_with_buffer (pixel_buffer, view->prompt_box,
                                                   view->prompt_box_area.x, view->prompt_box_area.y);
                ply_label_draw_area (view->prompt_label, pixel_buffer, x, y, width, height);
                ply_label_draw_area (view->entry_label, pixel_buffer, x, y, width, height);
        }
}

/* ---------- Boot messages ---------- */

static void
update_log (ply_boot_splash_plugin_t *plugin)
{
        ply_list_node_t *node;
        bool had_text = plugin->log_has_text;

        plugin->update_queued = false;
        plugin->log_has_text = get_last_log_line (plugin) >= 0;

        ply_list_foreach (plugin->views, node) {
                view_t *view = ply_list_node_get_data (node);

                ply_pixel_display_pause_updates (view->display);
                view_update_log (view);
                if (plugin->log_has_text != had_text) {
                        /* The box appears and the prompt box moves below it */
                        if (plugin->state != PLY_BOOT_SPLASH_DISPLAY_NORMAL)
                                view_place_prompt (view);
                        if (plugin->is_visible)
                                view_redraw (view);
                } else if (plugin->is_visible) {
                        view_redraw_area (view, &view->log_box_area);
                }
                ply_pixel_display_unpause_updates (view->display);
        }
}

static void
on_update_timeout (void             *user_data,
                   ply_event_loop_t *loop)
{
        update_log (user_data);
}

static void
on_terminal_output (void       *user_data,
                    const char *output)
{
        ply_boot_splash_plugin_t *plugin = user_data;

        if (plugin->update_queued || plugin->loop == NULL)
                return;

        plugin->update_queued = true;
        ply_event_loop_watch_for_timeout (plugin->loop, UPDATE_INTERVAL,
                                          on_update_timeout, plugin);
}

static void
on_boot_output (ply_boot_splash_plugin_t *plugin,
                const char               *output,
                size_t                    size)
{
        if (!plugin->show_log)
                return;

        ply_terminal_emulator_parse_lines (plugin->terminal, output, size);
}

/* ---------- Prompts ---------- */

static void
show_prompt (ply_boot_splash_plugin_t *plugin)
{
        ply_list_node_t *node;
        /* "●" is three bytes in UTF-8, then the cursor */
        char bullet_text[MAX_BULLETS * 3 + 2];
        char *question_text = NULL;
        const char *entry;

        if (plugin->state == PLY_BOOT_SPLASH_DISPLAY_PASSWORD_ENTRY) {
                int bullets = MIN (MAX (plugin->bullets, 0), MAX_BULLETS);

                for (int i = 0; i < bullets; i++)
                        memcpy (bullet_text + i * 3, "\xe2\x97\x8f", 3);
                strcpy (bullet_text + bullets * 3, "_");
                entry = bullet_text;
        } else {
                question_text = format_string ("%s_", plugin->entry_text != NULL ? plugin->entry_text : "");
                if (question_text == NULL)
                        return;
                entry = question_text;
        }

        ply_list_foreach (plugin->views, node) {
                view_t *view = ply_list_node_get_data (node);

                ply_pixel_display_pause_updates (view->display);
                ply_label_set_text (view->prompt_label, plugin->prompt != NULL ? plugin->prompt : "");
                ply_label_set_text (view->entry_label, entry);
                view_place_prompt (view);
                if (plugin->is_visible)
                        view_redraw (view);
                ply_pixel_display_unpause_updates (view->display);
        }

        free (question_text);
}

static void
hide_prompt (ply_boot_splash_plugin_t *plugin)
{
        ply_list_node_t *node;

        ply_list_foreach (plugin->views, node) {
                view_t *view = ply_list_node_get_data (node);

                ply_label_hide (view->prompt_label);
                ply_label_hide (view->entry_label);
        }
}

/* Refills the message rows after the number of visible ones changed */
static void
refill_log_rows (ply_boot_splash_plugin_t *plugin)
{
        ply_list_node_t *node;

        if (!plugin->show_log)
                return;

        ply_list_foreach (plugin->views, node) {
                view_update_log (ply_list_node_get_data (node));
        }
}

static void
set_prompt (ply_boot_splash_plugin_t      *plugin,
            ply_boot_splash_display_type_t state,
            const char                    *prompt,
            const char                    *entry_text,
            int                            bullets)
{
        bool was_normal = plugin->state == PLY_BOOT_SPLASH_DISPLAY_NORMAL;

        plugin->state = state;
        if (was_normal)
                refill_log_rows (plugin);
        free (plugin->prompt);
        plugin->prompt = prompt != NULL ? strdup (prompt) : NULL;
        free (plugin->entry_text);
        plugin->entry_text = entry_text != NULL ? strdup (entry_text) : NULL;
        plugin->bullets = bullets;

        show_prompt (plugin);
}

static void
display_normal (ply_boot_splash_plugin_t *plugin)
{
        if (plugin->state == PLY_BOOT_SPLASH_DISPLAY_NORMAL)
                return;

        plugin->state = PLY_BOOT_SPLASH_DISPLAY_NORMAL;
        hide_prompt (plugin);
        refill_log_rows (plugin);
        redraw_views (plugin);
}

static void
display_password (ply_boot_splash_plugin_t *plugin,
                  const char               *prompt,
                  int                       bullets)
{
        set_prompt (plugin, PLY_BOOT_SPLASH_DISPLAY_PASSWORD_ENTRY, prompt, NULL, bullets);
}

static void
display_question (ply_boot_splash_plugin_t *plugin,
                  const char               *prompt,
                  const char               *entry_text)
{
        set_prompt (plugin, PLY_BOOT_SPLASH_DISPLAY_QUESTION_ENTRY, prompt, entry_text, -1);
}

/* Messages such as "Press Ctrl+C to cancel all filesystem checks" go into
 * the message box, like on the console. */
static void
display_message (ply_boot_splash_plugin_t *plugin,
                 const char               *message)
{
        int current;

        if (!plugin->show_log || message == NULL)
                return;

        current = ply_terminal_emulator_get_line_count (plugin->terminal) - 1;
        if (ply_rich_text_get_length (ply_terminal_emulator_get_nth_line (plugin->terminal, current)) > 0)
                ply_terminal_emulator_parse_lines (plugin->terminal, "\n", 1);
        ply_terminal_emulator_parse_lines (plugin->terminal, message, strlen (message));
        ply_terminal_emulator_parse_lines (plugin->terminal, "\n", 1);
}

/* ---------- Offline updates ---------- */

static void
system_update (ply_boot_splash_plugin_t *plugin,
               int                       progress)
{
        ply_list_node_t *node;

        free (plugin->status);
        plugin->status = format_string ("%s %d%%",
                  plugin->mode == PLY_BOOT_SPLASH_MODE_FIRMWARE_UPGRADE ? "Updating firmware" :
                  plugin->mode == PLY_BOOT_SPLASH_MODE_SYSTEM_UPGRADE ? "Upgrading the system" :
                  "Installing updates", CLAMP (progress, 0, 100));

        ply_list_foreach (plugin->views, node) {
                view_t *view = ply_list_node_get_data (node);

                view_show_status (view);
                if (plugin->is_visible)
                        view_redraw (view);
        }
}

/* ---------- Plugin ---------- */

static ply_boot_splash_plugin_t *
create_plugin (ply_key_file_t *key_file)
{
        ply_boot_splash_plugin_t *plugin;

        plugin = calloc (1, sizeof(ply_boot_splash_plugin_t));

        plugin->image_dir = key_file_get_string (key_file, "ImageDir", "/usr/share/plymouth/themes/leios");

        /* Logo height and the height of its centre, as fractions of the screen height */
        plugin->logo_size = ply_key_file_get_double (key_file, "leios", "LogoSize", 0.08);
        plugin->logo_position = ply_key_file_get_double (key_file, "leios", "LogoVerticalPosition", 0.10);
        /* Message box below the logo: its width as a fraction of the screen
         * width, where it ends as a fraction of the screen height, background
         * colour (0xRRGGBB) and its opacity */
        plugin->box_width = ply_key_file_get_double (key_file, "leios", "MessageBoxWidth", 0.80);
        plugin->box_bottom = ply_key_file_get_double (key_file, "leios", "MessageBoxBottom", 0.95);
        plugin->box_color = key_file_get_color (key_file, "MessageBoxColor", 0x0d1526);
        plugin->box_opacity = CLAMP (ply_key_file_get_double (key_file, "leios", "MessageBoxOpacity", 0.55), 0.0, 1.0);
        /* Font families (Pango syntax, without the size), the font size for a
         * 1080 pixel high screen and the text colour (0xRRGGBBAA) */
        plugin->font = key_file_get_string (key_file, "Font", "Geist, sans-serif");
        plugin->monospace_font = key_file_get_string (key_file, "MonospaceFont", "Geist Mono, monospace");
        plugin->font_size = ply_key_file_get_double (key_file, "leios", "FontSize", 11.0);
        plugin->text_color = key_file_get_color (key_file, "TextColor", 0xe8eef7ff);

        plugin->terminal = ply_terminal_emulator_new (TERMINAL_ROWS, TERMINAL_COLUMNS);
        ply_terminal_emulator_watch_for_output (plugin->terminal, on_terminal_output, plugin);

        plugin->views = ply_list_new ();
        plugin->state = PLY_BOOT_SPLASH_DISPLAY_NORMAL;

        return plugin;
}

static void
on_event_loop_exit (void             *user_data,
                    int               exit_code,
                    ply_event_loop_t *loop)
{
        ply_boot_splash_plugin_t *plugin = user_data;

        plugin->loop = NULL;
}

static void
stop_watching_event_loop (ply_boot_splash_plugin_t *plugin)
{
        if (plugin->loop == NULL)
                return;

        if (plugin->update_queued) {
                ply_event_loop_stop_watching_for_timeout (plugin->loop, on_update_timeout, plugin);
                plugin->update_queued = false;
        }
        ply_event_loop_stop_watching_for_exit (plugin->loop, on_event_loop_exit, plugin);
        plugin->loop = NULL;
}

static void
destroy_plugin (ply_boot_splash_plugin_t *plugin)
{
        if (plugin == NULL)
                return;

        stop_watching_event_loop (plugin);
        free_views (plugin);
        ply_list_free (plugin->views);

        if (plugin->background_image != NULL)
                ply_image_free (plugin->background_image);
        if (plugin->logo_image != NULL)
                ply_image_free (plugin->logo_image);
        ply_terminal_emulator_free (plugin->terminal);

        free (plugin->image_dir);
        free (plugin->font);
        free (plugin->monospace_font);
        free (plugin->prompt);
        free (plugin->entry_text);
        free (plugin->status);
        free (plugin);
}

static void
add_pixel_display (ply_boot_splash_plugin_t *plugin,
                   ply_pixel_display_t      *display)
{
        view_t *view;

        view = view_new (plugin, display);
        ply_pixel_display_set_draw_handler (view->display, on_draw, view);

        if (plugin->is_visible && !view_load (view)) {
                ply_pixel_display_set_draw_handler (view->display, NULL, NULL);
                view_free (view);
                return;
        }

        ply_list_append_data (plugin->views, view);

        if (plugin->is_visible) {
                if (plugin->state != PLY_BOOT_SPLASH_DISPLAY_NORMAL)
                        show_prompt (plugin);
                view_redraw (view);
        }
}

static void
remove_pixel_display (ply_boot_splash_plugin_t *plugin,
                      ply_pixel_display_t      *display)
{
        ply_list_node_t *node;

        ply_list_foreach (plugin->views, node) {
                view_t *view = ply_list_node_get_data (node);

                if (view->display == display) {
                        ply_pixel_display_set_draw_handler (view->display, NULL, NULL);
                        view_free (view);
                        ply_list_remove_node (plugin->views, node);
                        return;
                }
        }
}

static bool
show_splash_screen (ply_boot_splash_plugin_t *plugin,
                    ply_event_loop_t         *loop,
                    ply_buffer_t             *boot_buffer,
                    ply_boot_splash_mode_t    mode)
{
        assert (plugin != NULL);

        plugin->loop = loop;
        plugin->mode = mode;

        if (plugin->background_image == NULL)
                plugin->background_image = load_image (plugin->image_dir, "background.png");
        if (plugin->logo_image == NULL)
                plugin->logo_image = load_image (plugin->image_dir, "logo.png");

        /* Same check as Plymouth's console viewer: text rendering works and
         * plymouth.prefer-fbcon is not on the kernel command line. */
        plugin->show_log = ply_console_viewer_preferred ();
        if (plugin->show_log && boot_buffer != NULL)
                ply_terminal_emulator_convert_boot_buffer (plugin->terminal, boot_buffer);

        load_views (plugin);
        if (plugin->update_queued) {
                /* Show the earlier output right away rather than next frame */
                ply_event_loop_stop_watching_for_timeout (loop, on_update_timeout, plugin);
                update_log (plugin);
        }

        ply_event_loop_watch_for_exit (loop, on_event_loop_exit, plugin);

        plugin->is_visible = true;
        redraw_views (plugin);

        return true;
}

static void
hide_splash_screen (ply_boot_splash_plugin_t *plugin,
                    ply_event_loop_t         *loop)
{
        assert (plugin != NULL);

        stop_watching_event_loop (plugin);
        plugin->is_visible = false;
}

static void
update_status (ply_boot_splash_plugin_t *plugin,
               const char               *status)
{
}

static void
become_idle (ply_boot_splash_plugin_t *plugin,
             ply_trigger_t            *idle_trigger)
{
        /* Nothing is animated, so there is nothing to wind down. */
        ply_trigger_pull (idle_trigger, NULL);
}

ply_boot_splash_plugin_interface_t *
ply_boot_splash_plugin_get_interface (void)
{
        static ply_boot_splash_plugin_interface_t plugin_interface =
        {
                .create_plugin        = create_plugin,
                .destroy_plugin       = destroy_plugin,
                .add_pixel_display    = add_pixel_display,
                .remove_pixel_display = remove_pixel_display,
                .show_splash_screen   = show_splash_screen,
                .update_status        = update_status,
                .on_boot_output       = on_boot_output,
                .hide_splash_screen   = hide_splash_screen,
                .display_normal       = display_normal,
                .display_password     = display_password,
                .display_question     = display_question,
                .display_message      = display_message,
                .system_update        = system_update,
                .become_idle          = become_idle,
        };

        return &plugin_interface;
}
