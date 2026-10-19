/*
 * fcitx4 GTK4 IM module - based on fcitx-4.2.9.8 gtk2/gtk3 frontend
 * Copyright (C) 2010~2012 CSSlayer <wengxt@gmail.com>
 * Copyright (C) 2026 ipfgao <ipfgao@hotmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "fcitx-utils/utils.h"
#include "fcitx/frontend.h"
#include "fcitx/keys.h"
#include "fcitx-gclient/fcitxclient.h"
#include "fcitx-utils/utf8.h"
#include "fcitximcontext.h"

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#ifndef GDK_WINDOWING_X11
#define GDK_WINDOWING_X11 1
#endif
#include <gdk/x11/gdkx.h>
#include <X11/Xlib.h>
#include <string.h>

#define FCITX_UNUSED(x) (void)(x)

#define FcitxKeyState_HandledMask (1 << 24)
#define FcitxKeyState_IgnoredMask (1 << 25)

#define MAX_CACHED_HANDLED_EVENT 80


/* forward declarations */
static gboolean _set_cursor_location_internal(FcitxIMContext *fcitxcontext);
static void _request_surrounding_text(FcitxIMContext **context);
static gboolean _request_surrounding_text_after_focus(
    FcitxIMContext *fcitxcontext);

static void fcitx_im_context_finalize(GObject *object);

static guint fcitx_im_context_klass_signal_commit = 0;

static FcitxConnection *fcitx4_connection = NULL;
static guint fcitx_im_context_klass_signal_retrieve = 0;
static guint fcitx_im_context_klass_signal_delete = 0;
static guint fcitx_im_context_klass_signal_preedit_changed = 0;
static guint fcitx_im_context_klass_signal_preedit_start = 0;
static guint fcitx_im_context_klass_signal_preedit_end = 0;

static void _fcitx_im_context_connect_cb(FcitxClient *im, void *user_data);
static void _fcitx_im_context_enable_im_cb(FcitxClient *im, void *user_data);
static void _fcitx_im_context_close_im_cb(FcitxClient *im, void *user_data);
static void _fcitx_im_context_commit_string_cb(FcitxClient *im,
                                               const char *str,
                                               void *user_data);
static void _fcitx_im_context_update_formatted_preedit_cb(
    FcitxClient *im, GPtrArray *preedit, gint cursor_pos, void *user_data);
static void _fcitx_im_context_forward_key_cb(FcitxClient *im, guint keyval,
                                             guint state, gint type,
                                             void *user_data);
static void _fcitx_im_context_delete_surrounding_cb(FcitxClient *im,
                                                    gint offset,
                                                    guint nchars,
                                                    void *user_data);

static GObjectClass *fcitx_im_context_parent_class;

static GtkIMContext *_focus_im_context = NULL;

/* xkb compose */
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>

typedef struct _ComposeData {
    struct xkb_context *ctx;
    struct xkb_compose_table *table;
    struct xkb_compose_state *state;
} ComposeData;

G_LOCK_DEFINE_STATIC(snooper_lock);

static void _fcitx_im_context_delete_surrounding_cb(
    FcitxClient *im, gint offset_from_cursor, guint nchars, void *user_data) {
    FCITX_UNUSED(im);
    FcitxIMContext *context = FCITX_IM_CONTEXT(user_data);
    gboolean return_value;
    g_signal_emit(context, context->signal_delete_surrounding_id, 0,
                  offset_from_cursor, nchars, &return_value);
}

typedef struct _ForwardKeyItem {
    guint keyval;
    guint state;
    gint type;
} ForwardKeyItem;

/* preedit update */
/* forward key (GTK4: 存入待转发队列, filter_keypress 匹配时交给 slave) */
static void _fcitx_im_context_forward_key_cb(FcitxClient *im, guint keyval,
                                             guint state, gint type,
                                             void *user_data) {
    FCITX_UNUSED(im);
    FcitxIMContext *context = FCITX_IM_CONTEXT(user_data);
    FcitxLog(DEBUG, "_fcitx_im_context_forward_key_cb: keyval=0x%x type=%d",
             keyval, type);

    ForwardKeyItem *item = g_new0(ForwardKeyItem, 1);
    item->keyval = keyval;
    item->state = state;
    item->type = type;
    g_queue_push_tail(context->forward_pending, item);
}

/* --- slave (GtkIMContextSimple) 信号桥接：fallback 输入 → 应用 --- */
static void _slave_commit_cb(GtkIMContext *slave, const gchar *str,
                             FcitxIMContext *context) {
    FCITX_UNUSED(slave);
    /* 只有 filter 走到 fallback/forward 时 slave 才产生 commit，
       此时 fcitx 已明确未处理该键，必须无条件转发给应用 */
    g_signal_emit(context, context->signal_commit_id, 0, str);
}

static void _slave_preedit_start_cb(GtkIMContext *slave,
                                    FcitxIMContext *context) {
    FCITX_UNUSED(slave);
    g_signal_emit(context, context->signal_preedit_start_id, 0);
}

static void _slave_preedit_end_cb(GtkIMContext *slave,
                                  FcitxIMContext *context) {
    FCITX_UNUSED(slave);
    g_signal_emit(context, context->signal_preedit_end_id, 0);
}

static void _slave_preedit_changed_cb(GtkIMContext *slave,
                                      FcitxIMContext *context) {
    FCITX_UNUSED(slave);
    g_signal_emit(context, context->signal_preedit_changed_id, 0);
}

static void _forward_key_item_free(gpointer data) {
    g_free(data);
}

/* process key callback (async) */
static void _fcitx_im_context_process_key_cb(GObject *sourceobject,
                                             GAsyncResult *res,
                                             gpointer user_data) {
    gpointer *p = user_data;
    GdkEvent *event = (GdkEvent *)p[0];
    FcitxIMContext *context = (FcitxIMContext *)p[1];
    gboolean ret;

    if (!context || !event) {
        if (event)
            gdk_event_unref(event);
        if (context)
            g_object_unref(context);
        g_free(user_data);
        return;
    }

    ret = fcitx_client_process_key_finish(FCITX_CLIENT(sourceobject), res);
    if (ret) {
        if (!g_hash_table_contains(context->handled_events, event)) {
            g_hash_table_add(context->handled_events,
                             gdk_event_ref(event));
            g_queue_push_tail(context->handled_events_list, event);
            while (g_hash_table_size(context->handled_events) >
                   MAX_CACHED_HANDLED_EVENT) {
                g_hash_table_remove(
                    context->handled_events,
                    g_queue_pop_head(context->handled_events_list));
            }
        }
        g_hash_table_remove(context->pending_events, event);
    } else {
        /* fcitx 未处理：登记到 reinjected_events 后放回事件队列，
           filter_keypress 入口会命中并交给 GTK/应用处理（防重入死循环） */
        g_hash_table_remove(context->pending_events, event);
        if (!g_hash_table_contains(context->reinjected_events, event))
            g_hash_table_add(context->reinjected_events,
                             gdk_event_ref(event));
        GdkDisplay *display = gdk_event_get_display(event);
        if (display)
            gdk_display_put_event(display, event);
    }

    gdk_event_unref(event);
    g_object_unref(context);
    g_free(user_data);
}

/* XKB compose table setup (from upstream fcitx4 keys) */
static struct xkb_compose_table *_xkb_compose_table_new(
    struct xkb_context *ctx, const char *locale) {
    struct xkb_compose_table *table =
        xkb_compose_table_new_from_locale(ctx, locale, XKB_COMPOSE_COMPILE_NO_FLAGS);
    if (!table) {
        table = xkb_compose_table_new_from_locale(ctx, "C",
            XKB_COMPOSE_COMPILE_NO_FLAGS);
    }
    return table;
}

static ComposeData *_compose_data_new(void) {
    ComposeData *data = g_new0(ComposeData, 1);
    data->ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!data->ctx) {
        g_free(data);
        return NULL;
    }
    const char *locale = g_getenv("LC_ALL");
    if (!locale)
        locale = g_getenv("LC_CTYPE");
    if (!locale)
        locale = g_getenv("LANG");
    if (!locale)
        locale = "C";
    data->table = _xkb_compose_table_new(data->ctx, locale);
    if (!data->table) {
        xkb_context_unref(data->ctx);
        g_free(data);
        return NULL;
    }
    data->state = xkb_compose_state_new(data->table, XKB_COMPOSE_STATE_NO_FLAGS);
    if (!data->state) {
        xkb_compose_table_unref(data->table);
        xkb_context_unref(data->ctx);
        g_free(data);
        return NULL;
    }
    return data;
}

static void _compose_data_free(ComposeData *data) {
    if (!data)
        return;
    if (data->state)
        xkb_compose_state_unref(data->state);
    if (data->table)
        xkb_compose_table_unref(data->table);
    if (data->ctx)
        xkb_context_unref(data->ctx);
    g_free(data);
}

static GList *compose_data_list;
static G_LOCK_DEFINE(compose_lock);

static ComposeData *_compose_data_get(void) {
    G_LOCK(compose_lock);
    ComposeData *data = g_list_nth_data(compose_data_list, 0);
    if (!data) {
        data = _compose_data_new();
        if (data)
            compose_data_list = g_list_prepend(compose_data_list, data);
    }
    G_UNLOCK(compose_lock);
    return data;
}

static struct xkb_compose_state *_compose_state_get(void) {
    ComposeData *data = _compose_data_get();
    if (!data)
        return NULL;
    return data->state;
}

/* set client widget (GTK4 API) */
static void set_ic_client_widget(FcitxIMContext *context,
                                 GtkWidget *client_widget) {
    if (context->client_widget)
        g_object_unref(context->client_widget);
    context->client_widget = NULL;

    if (client_widget != NULL)
        context->client_widget = g_object_ref(client_widget);

    gtk_im_context_set_client_widget(context->slave, client_widget);
}

static void fcitx_im_context_set_client_widget(GtkIMContext *context,
                                               GtkWidget *client_widget) {
    FcitxIMContext *fcitxcontext = FCITX_IM_CONTEXT(context);
    FcitxLog(DEBUG, "fcitx_im_context_set_client_widget");
    set_ic_client_widget(fcitxcontext, client_widget);
}

static void fcitx_im_context_get_preedit_string(GtkIMContext *context,
                                                gchar **str,
                                                PangoAttrList **attrs,
                                                gint *cursor_pos) {
    FcitxIMContext *fcitxcontext = FCITX_IM_CONTEXT(context);

    if (str)
        *str = g_strdup(fcitxcontext->preedit_string ?
                            fcitxcontext->preedit_string : "");
    if (attrs)
        *attrs = (fcitxcontext->attrlist)
                     ? pango_attr_list_ref(fcitxcontext->attrlist)
                     : pango_attr_list_new();
    if (cursor_pos)
        *cursor_pos = fcitxcontext->cursor_pos;
}

static gboolean fcitx_im_context_get_surrounding_with_selection(
    GtkIMContext *context, gchar **text, gint *cursor_index,
    gint *anchor_index) {
    FcitxIMContext *fcitxcontext = FCITX_IM_CONTEXT(context);
    gboolean ret = gtk_im_context_get_surrounding_with_selection(
        fcitxcontext->slave, text, cursor_index, anchor_index);
    if (ret) {
        FcitxLog(DEBUG, "get_surrounding_with_selection: %s", *text);
    }
    return ret;
}

static gboolean fcitx_im_context_delete_surrounding(GtkIMContext *context,
                                                     gint offset, gint nchars) {
    FcitxIMContext *fcitxcontext = FCITX_IM_CONTEXT(context);
    gboolean ret =
        gtk_im_context_delete_surrounding(fcitxcontext->slave, offset, nchars);
    /* GTK3 参考版：slave 删除失败时尝试从 fcitx 侧请求后重删，此处保持 slave 结果 */
    return ret;
}

static int get_selection_anchor_point(FcitxIMContext *fcitxcontext,
                                      guint cursor_pos,
                                      guint surrounding_text_len) {
    GtkWidget *widget = fcitxcontext->client_widget;
    if (widget == NULL) {
        return cursor_pos;
    }
    if (!GTK_IS_TEXT_VIEW(widget)) {
        return cursor_pos;
    }

    GtkTextView *text_view = GTK_TEXT_VIEW(widget);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(text_view);

    if (!gtk_text_buffer_get_has_selection(buffer)) {
        return cursor_pos;
    }

    GtkTextIter start_iter, end_iter, cursor_iter;
    if (!gtk_text_buffer_get_selection_bounds(buffer, &start_iter, &end_iter)) {
        return cursor_pos;
    }

    gtk_text_buffer_get_iter_at_mark(buffer, &cursor_iter,
                                     gtk_text_buffer_get_insert(buffer));

    guint start_index = gtk_text_iter_get_offset(&start_iter);
    guint end_index = gtk_text_iter_get_offset(&end_iter);
    guint cursor_index = gtk_text_iter_get_offset(&cursor_iter);

    guint anchor;
    if (start_index == cursor_index) {
        anchor = end_index;
    } else if (end_index == cursor_index) {
        anchor = start_index;
    } else {
        return cursor_pos;
    }

    guint relative_origin = cursor_index - cursor_pos;
    if (anchor < relative_origin) {
        return cursor_pos;
    }
    anchor -= relative_origin;

    if (anchor > surrounding_text_len) {
        return cursor_pos;
    }
    return anchor;
}

static void fcitx_im_context_set_surrounding_with_selection(
    GtkIMContext *context, const char *text, int l, int cursor_index,
    int anchor_index) {
    FcitxIMContext *fcitxcontext = FCITX_IM_CONTEXT(context);

    int len = l;
    if (len < 0) {
        len = strlen(text);
    }
    if (0 <= cursor_index && cursor_index <= len &&
        fcitx_client_is_valid(fcitxcontext->client)) {
        gchar *p = g_strndup(text, len);
        int cursor_pos = g_utf8_strlen(p, cursor_index);
        int utf8_len = g_utf8_strlen(p, len);

        gint anchor_pos =
            (anchor_index == cursor_index)
                ? get_selection_anchor_point(fcitxcontext, cursor_pos,
                                             utf8_len)
                : g_utf8_strlen(p, anchor_index);

        /* 去重：文本未变且位置未变时不重复发送 DBus（对齐 GTK3 参考版） */
        gboolean text_changed = (g_strcmp0(fcitxcontext->surrounding_text, p) != 0);
        if (text_changed) {
            g_free(fcitxcontext->surrounding_text);
            fcitxcontext->surrounding_text = p;
        } else {
            g_free(p);
        }

        if (text_changed || fcitxcontext->last_cursor_pos != cursor_pos ||
            fcitxcontext->last_anchor_pos != anchor_pos) {
            fcitxcontext->last_cursor_pos = cursor_pos;
            fcitxcontext->last_anchor_pos = anchor_pos;
            fcitx_client_set_surrounding_text(fcitxcontext->client,
                                              g_strndup(text, len),
                                              cursor_pos, anchor_pos);
        }
    }
    gtk_im_context_set_surrounding_with_selection(fcitxcontext->slave, text,
                                                  l, cursor_index,
                                                  anchor_index);
}

static void fcitx_im_context_set_use_preedit(GtkIMContext *context,
                                             gboolean use_preedit) {
    FcitxIMContext *fcitxcontext = FCITX_IM_CONTEXT(context);
    fcitxcontext->use_preedit = use_preedit;
    gtk_im_context_set_use_preedit(fcitxcontext->slave, use_preedit);
}

static void fcitx_im_context_focus_in(GtkIMContext *context) {
    FcitxIMContext *fcitxcontext = FCITX_IM_CONTEXT(context);
    FcitxLog(DEBUG, "fcitx_im_context_focus_in");

    if (fcitxcontext->has_focus)
        return;

    fcitxcontext->has_focus = TRUE;

    if (fcitx_client_is_valid(fcitxcontext->client)) {
        fcitx_client_focus_in(fcitxcontext->client);
    }

    gtk_im_context_focus_in(fcitxcontext->slave);

    /* set_cursor_location_internal() gets origin from X server, blocks UI.
     * Delay it to idle callback. */
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                    (GSourceFunc)_set_cursor_location_internal,
                    g_object_ref(fcitxcontext),
                    (GDestroyNotify)g_object_unref);

    /* delay surrounding text request */
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                    (GSourceFunc)_request_surrounding_text_after_focus,
                    g_object_ref(fcitxcontext),
                    (GDestroyNotify)g_object_unref);

    g_object_add_weak_pointer((GObject *)context, (gpointer *)&_focus_im_context);
    _focus_im_context = context;
}

static void fcitx_im_context_focus_out(GtkIMContext *context) {
    FcitxIMContext *fcitxcontext = FCITX_IM_CONTEXT(context);
    FcitxLog(DEBUG, "fcitx_im_context_focus_out");

    if (!fcitxcontext->has_focus)
        return;

    if (_focus_im_context == context)
        _focus_im_context = NULL;
    g_object_remove_weak_pointer((GObject *)context,
                                 (gpointer *)&_focus_im_context);

    fcitxcontext->has_focus = FALSE;

    if (fcitx_client_is_valid(fcitxcontext->client)) {
        fcitx_client_focus_out(fcitxcontext->client);
    }

    fcitxcontext->cursor_pos = 0;
    if (fcitxcontext->preedit_string != NULL) {
        g_free(fcitxcontext->preedit_string);
        fcitxcontext->preedit_string = NULL;
        fcitxcontext->preedit_visible = FALSE;
        g_signal_emit(fcitxcontext, fcitxcontext->signal_preedit_changed_id, 0);
        g_signal_emit(fcitxcontext, fcitxcontext->signal_preedit_end_id, 0);
    }

    gtk_im_context_focus_out(fcitxcontext->slave);
}

/* set cursor location (GTK4: surface coords) */
static gboolean _set_cursor_location_internal(FcitxIMContext *fcitxcontext) {
    GdkRectangle area;

    if (fcitxcontext->client_widget == NULL ||
        !fcitx_client_is_valid(fcitxcontext->client)) {
        return FALSE;
    }

    area = fcitxcontext->area;

    if (area.x == -1 && area.y == -1 && area.width == 0 && area.height == 0) {
        area.x = 0;
        area.y += 0;
    }

    /* translate widget coords to native surface top-level coords */
    GtkWidget *widget = fcitxcontext->client_widget;
    GtkNative *native = gtk_widget_get_native(widget);
    if (native != NULL) {
        GdkSurface *surface = gtk_native_get_surface(native);
        if (surface) {
            int scale = gdk_surface_get_scale_factor(surface);
            gdouble px = 0, py = 0;
            gtk_widget_translate_coordinates(widget, GTK_WIDGET(native),
                                             area.x, area.y, &px, &py);
            area.x = (int)(px * scale);
            area.y = (int)(py * scale);
            area.width *= scale;
            area.height *= scale;

            /* X11: convert to root coordinates so the candidate window
             * follows the cursor (same as fcitx5-gtk4) */
            GdkDisplay *display = gtk_widget_get_display(widget);
            if (GDK_IS_X11_DISPLAY(display)) {
                Window child;
                int root_x, root_y;
                Display *xdpy = gdk_x11_display_get_xdisplay(display);
                Window root =
                    gdk_x11_display_get_xrootwindow(display);
                Window xwin = gdk_x11_surface_get_xid(surface);
                if (xdpy && xwin) {
                    if (XTranslateCoordinates(xdpy, xwin, root, area.x, area.y,
                                              &root_x, &root_y, &child)) {
                        area.x = root_x;
                        area.y = root_y;
                    }
                }
            }
            fcitx_client_set_cursor_rect(fcitxcontext->client, area.x, area.y,
                                         area.width, area.height);
        }
    }
    return FALSE;
}

/* surrounding text helpers */
static void _request_surrounding_text(FcitxIMContext **context) {
    FcitxIMContext *c = *context;
    if (c && fcitx_client_is_valid(c->client) && c->has_focus) {
        gboolean return_value;

        /* 弱引用指向结构体成员（surrounding_weak_self），而非栈变量 */
        c->surrounding_weak_self = c;
        g_object_add_weak_pointer((GObject *)c,
                                  (gpointer *)&c->surrounding_weak_self);
        g_signal_emit(c, c->signal_retrieve_surrounding_id, 0,
                      &return_value);
        if (c->surrounding_weak_self != NULL) {
            g_object_remove_weak_pointer(
                (GObject *)c, (gpointer *)&c->surrounding_weak_self);
            c->surrounding_weak_self = NULL;
            c->support_surrounding_text = return_value;
        }
        /* 若 context 在信号处理期间被销毁，weak_self 已为 NULL，不再访问 */
    }
}

static gboolean _request_surrounding_text_after_focus(
    FcitxIMContext *fcitxcontext) {
    _request_surrounding_text(&fcitxcontext);
    return FALSE;
}

/* fallback: fcitx 未处理/不可用时, 走 xkb compose + GtkIMContextSimple */
static gboolean fcitx_im_context_filter_keypress_fallback(
    FcitxIMContext *context, GdkEvent *event) {
    struct xkb_compose_state *xkbComposeState = _compose_state_get();
    if (!xkbComposeState || gdk_event_get_event_type(event) != GDK_KEY_PRESS) {
        return gtk_im_context_filter_keypress(context->slave, event);
    }

    guint keyval = gdk_key_event_get_keyval(event);
    enum xkb_compose_feed_result result =
        xkb_compose_state_feed(xkbComposeState, keyval);
    if (result == XKB_COMPOSE_FEED_IGNORED) {
        return gtk_im_context_filter_keypress(context->slave, event);
    }

    enum xkb_compose_status status =
        xkb_compose_state_get_status(xkbComposeState);
    if (status == XKB_COMPOSE_NOTHING) {
        return gtk_im_context_filter_keypress(context->slave, event);
    } else if (status == XKB_COMPOSE_COMPOSED) {
        char buffer[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        int length = xkb_compose_state_get_utf8(xkbComposeState, buffer,
                                                sizeof(buffer));
        xkb_compose_state_reset(xkbComposeState);
        if (length != 0) {
            g_signal_emit(context, context->signal_commit_id, 0, buffer);
        }
    } else if (status == XKB_COMPOSE_CANCELLED) {
        xkb_compose_state_reset(xkbComposeState);
    }

    return TRUE;
}

/* filter keypress (GTK4) */
static gboolean fcitx_im_context_filter_keypress(GtkIMContext *context,
                                                 GdkEvent *event) {
    FcitxIMContext *fcitxcontext = FCITX_IM_CONTEXT(context);

    /* 重注入事件：不再次进入 fcitx，直接交给 GTK 派发（防死循环） */
    if (event != NULL &&
        g_hash_table_remove(fcitxcontext->reinjected_events, event)) {
        return FALSE;
    }

    /* fcitx forward-key 转发：匹配当前事件的转发请求交给 slave 处理一次 */
    if (event != NULL && !g_queue_is_empty(fcitxcontext->forward_pending)) {
        guint cur_keyval = gdk_key_event_get_keyval(event);
        GList *head = g_queue_peek_head_link(fcitxcontext->forward_pending);
        if (head != NULL) {
            ForwardKeyItem *item = head->data;
            if (item != NULL && item->keyval == cur_keyval) {
                g_queue_pop_head(fcitxcontext->forward_pending);
                g_free(item);
                return gtk_im_context_filter_keypress(fcitxcontext->slave,
                                                      event);
            }
        }
    }

    if (g_hash_table_contains(fcitxcontext->handled_events, event)) {
        return TRUE;
    }
    if (g_hash_table_contains(fcitxcontext->pending_events, event)) {
        g_hash_table_remove(fcitxcontext->pending_events, event);
        return fcitx_im_context_filter_keypress_fallback(fcitxcontext, event);
    }

    if (fcitx_client_is_valid(fcitxcontext->client) &&
        fcitxcontext->has_focus && event != NULL) {
        gboolean is_press =
            (gdk_event_get_event_type(event) == GDK_KEY_PRESS);

        guint state = gdk_event_get_modifier_state(event);
        guint keyval = gdk_key_event_get_keyval(event);
        guint hardware_keycode = gdk_key_event_get_keycode(event);
        guint time = gdk_event_get_time(event);

        if (fcitxcontext->sync_mode) {
            gint ret = fcitx_client_process_key_sync(
                fcitxcontext->client, keyval, hardware_keycode, state,
                is_press ? FCITX_PRESS_KEY : FCITX_RELEASE_KEY, time);
            if (ret <= 0)
                return fcitx_im_context_filter_keypress_fallback(
                    fcitxcontext, event);
            else {
                g_hash_table_add(fcitxcontext->handled_events,
                                 gdk_event_ref(event));
                return TRUE;
            }
        } else {
            /* async: add to pending, let dbus callback handle */
            g_hash_table_add(fcitxcontext->pending_events,
                             gdk_event_ref(event));
            gpointer *ud = g_malloc0(2 * sizeof(gpointer));
            ud[0] = gdk_event_ref(event);
            ud[1] = g_object_ref(fcitxcontext);
            fcitx_client_process_key(
                fcitxcontext->client, keyval, hardware_keycode, state,
                is_press ? FCITX_PRESS_KEY : FCITX_RELEASE_KEY, time, -1,
                NULL, _fcitx_im_context_process_key_cb, ud);
            return TRUE;
        }
    }
    return fcitx_im_context_filter_keypress_fallback(fcitxcontext, event);
}

static void fcitx_im_context_reset(GtkIMContext *context) {
    FcitxIMContext *fcitxcontext = FCITX_IM_CONTEXT(context);
    gtk_im_context_reset(fcitxcontext->slave);
    if (fcitxcontext->preedit_string != NULL) {
        g_free(fcitxcontext->preedit_string);
        fcitxcontext->preedit_string = NULL;
        fcitxcontext->preedit_visible = FALSE;
        g_signal_emit(fcitxcontext, fcitxcontext->signal_preedit_changed_id, 0);
        g_signal_emit(fcitxcontext, fcitxcontext->signal_preedit_end_id, 0);
    }
}

static void fcitx_im_context_set_cursor_location_vfunc(GtkIMContext *context,
                                                       GdkRectangle *area) {
    FcitxIMContext *fcitxcontext = FCITX_IM_CONTEXT(context);
    GdkRectangle new_area;

    if (area != NULL) {
        new_area = *area;
    } else {
        new_area.x = new_area.y = new_area.width = new_area.height = 0;
    }

    /* 去重：坐标未变化则不重复发送 */
    if (fcitxcontext->area.x == new_area.x &&
        fcitxcontext->area.y == new_area.y &&
        fcitxcontext->area.width == new_area.width &&
        fcitxcontext->area.height == new_area.height) {
        return;
    }
    fcitxcontext->area = new_area;

    if (fcitxcontext->slave != NULL) {
        gtk_im_context_set_cursor_location(fcitxcontext->slave, &new_area);
    }
    _set_cursor_location_internal(fcitxcontext);
}

/* gtype registration */

static void fcitx_im_context_class_init(FcitxIMContextClass *klass) {
    GtkIMContextClass *im_context_class = GTK_IM_CONTEXT_CLASS(klass);
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);

    fcitx_im_context_parent_class = g_type_class_peek_parent(klass);

    gobject_class->finalize = fcitx_im_context_finalize;

    im_context_class->set_client_widget = fcitx_im_context_set_client_widget;
    im_context_class->get_preedit_string = fcitx_im_context_get_preedit_string;
    im_context_class->get_surrounding_with_selection = fcitx_im_context_get_surrounding_with_selection;
    im_context_class->delete_surrounding = fcitx_im_context_delete_surrounding;
    im_context_class->set_surrounding_with_selection = fcitx_im_context_set_surrounding_with_selection;
    im_context_class->set_use_preedit = fcitx_im_context_set_use_preedit;
    im_context_class->focus_in = fcitx_im_context_focus_in;
    im_context_class->focus_out = fcitx_im_context_focus_out;
    im_context_class->set_cursor_location = fcitx_im_context_set_cursor_location_vfunc;
    im_context_class->filter_keypress = fcitx_im_context_filter_keypress;
    im_context_class->reset = fcitx_im_context_reset;

    /* 这些信号由 GtkIMContext 父类定义，这里只查询 ID 供 g_signal_emit 使用 */
    GType this_type = G_OBJECT_CLASS_TYPE(klass);
    fcitx_im_context_klass_signal_commit =
        g_signal_lookup("commit", this_type);
    fcitx_im_context_klass_signal_retrieve =
        g_signal_lookup("retrieve-surrounding", this_type);
    fcitx_im_context_klass_signal_delete =
        g_signal_lookup("delete-surrounding", this_type);
    fcitx_im_context_klass_signal_preedit_changed =
        g_signal_lookup("preedit-changed", this_type);
    fcitx_im_context_klass_signal_preedit_start =
        g_signal_lookup("preedit-start", this_type);
    fcitx_im_context_klass_signal_preedit_end =
        g_signal_lookup("preedit-end", this_type);
}

static void fcitx_im_context_init(FcitxIMContext *context) {
    context->slave = gtk_im_context_simple_new();
    g_signal_connect(G_OBJECT(context->slave), "commit",
                     G_CALLBACK(_slave_commit_cb), context);
    g_signal_connect(G_OBJECT(context->slave), "preedit-start",
                     G_CALLBACK(_slave_preedit_start_cb), context);
    g_signal_connect(G_OBJECT(context->slave), "preedit-end",
                     G_CALLBACK(_slave_preedit_end_cb), context);
    g_signal_connect(G_OBJECT(context->slave), "preedit-changed",
                     G_CALLBACK(_slave_preedit_changed_cb), context);
    context->use_preedit = TRUE;
    context->has_focus = FALSE;
    context->support_surrounding_text = FALSE;
    context->preedit_string = NULL;
    context->cursor_pos = 0;
    context->attrlist = NULL;
    context->sync_mode = TRUE;
    context->client_widget = NULL;
    context->client = NULL;
    context->surrounding_weak_self = NULL;
    context->xkbComposeState = NULL;

    context->pending_events =
        g_hash_table_new_full(g_direct_hash, g_direct_equal,
                              (GDestroyNotify)gdk_event_unref, NULL);
    context->handled_events =
        g_hash_table_new_full(g_direct_hash, g_direct_equal,
                              (GDestroyNotify)gdk_event_unref, NULL);
    context->handled_events_list = g_queue_new();
    context->forward_pending = g_queue_new();
    context->reinjected_events =
        g_hash_table_new_full(g_direct_hash, g_direct_equal,
                              (GDestroyNotify)gdk_event_unref, NULL);

    context->signal_commit_id = fcitx_im_context_klass_signal_commit;
    context->signal_retrieve_surrounding_id =
        fcitx_im_context_klass_signal_retrieve;
    context->signal_delete_surrounding_id =
        fcitx_im_context_klass_signal_delete;
    context->signal_preedit_changed_id =
        fcitx_im_context_klass_signal_preedit_changed;
    context->signal_preedit_start_id =
        fcitx_im_context_klass_signal_preedit_start;
    context->signal_preedit_end_id =
        fcitx_im_context_klass_signal_preedit_end;

    /* 全局单例连接（与 GTK3 前端一致），所有 context 共享 */
    static gsize init_done = 0;
    if (g_once_init_enter(&init_done)) {
        fcitx4_connection = fcitx_connection_new();
        g_object_ref_sink(fcitx4_connection);
        g_once_init_leave(&init_done, 1);
    }
    context->client =
        fcitx_client_new_with_connection(fcitx4_connection);
    g_object_set_data(G_OBJECT(context->client), "fcitx-im-context", context);

    g_signal_connect(G_OBJECT(context->client), "connected",
                     G_CALLBACK(_fcitx_im_context_connect_cb), context);
    g_signal_connect(G_OBJECT(context->client), "enable-im",
                     G_CALLBACK(_fcitx_im_context_enable_im_cb), context);
    g_signal_connect(G_OBJECT(context->client), "close-im",
                     G_CALLBACK(_fcitx_im_context_close_im_cb), context);
    g_signal_connect(G_OBJECT(context->client), "commit-string",
                     G_CALLBACK(_fcitx_im_context_commit_string_cb), context);
    g_signal_connect(G_OBJECT(context->client), "update-formatted-preedit",
                     G_CALLBACK(_fcitx_im_context_update_formatted_preedit_cb),
                     context);
    g_signal_connect(G_OBJECT(context->client), "forward-key",
                     G_CALLBACK(_fcitx_im_context_forward_key_cb), context);
    g_signal_connect(G_OBJECT(context->client), "delete-surrounding-text",
                     G_CALLBACK(_fcitx_im_context_delete_surrounding_cb),
                     context);

    fcitx_client_set_display(context->client,
                             gdk_display_get_name(gdk_display_get_default()));
}

/* module registration */
GType fcitx_im_context_register_type(GTypeModule *type_module) {
    static GType type = 0;
    if (type == 0) {
        static const GTypeInfo info = {
            sizeof(FcitxIMContextClass),
            NULL,
            NULL,
            (GClassInitFunc)fcitx_im_context_class_init,
            NULL,
            NULL,
            sizeof(FcitxIMContext),
            0,
            (GInstanceInitFunc)fcitx_im_context_init,
            NULL,
        };
        if (type_module) {
            type = g_type_module_register_type(type_module, GTK_TYPE_IM_CONTEXT,
                                               "FcitxIMContext", &info, 0);
        } else {
            type = g_type_register_static(GTK_TYPE_IM_CONTEXT,
                                          "FcitxIMContext", &info, 0);
        }
    }
    return type;
}

/*
 * 子类 GType，仅用于以 "fcitx4" 名字注册扩展点。
 * GLib 的 g_io_extension_point_implement 对同一 GType 只保留第一个名字，
 * 因此需要派生一个子类来获得独立 GType，使 GTK_IM_MODULE=fcitx4 也可用。
 * 所有行为（vfunc/init/finalize）继承自 FcitxIMContext。
 */
GType fcitx_im_context_register_type4(GTypeModule *type_module) {
    static GType type = 0;
    if (type == 0) {
        static const GTypeInfo info = {
            sizeof(FcitxIMContextClass),
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
            sizeof(FcitxIMContext),
            0,
            NULL,
            NULL,
        };
        if (type_module) {
            type = g_type_module_register_type(
                type_module, FCITX_TYPE_IM_CONTEXT, "FcitxIMContext4", &info, 0);
        } else {
            type = g_type_register_static(FCITX_TYPE_IM_CONTEXT,
                                          "FcitxIMContext4", &info, 0);
        }
    }
    return type;
}

/* connect callback */
static void _fcitx_im_context_connect_cb(FcitxClient *im, void *user_data) {
    FCITX_UNUSED(im);
    FcitxIMContext *context = FCITX_IM_CONTEXT(user_data);

    if (_focus_im_context == (GtkIMContext *)context &&
        fcitx_client_is_valid(context->client)) {
        fcitx_client_focus_in(context->client);
    }
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                    (GSourceFunc)_set_cursor_location_internal,
                    g_object_ref(context), (GDestroyNotify)g_object_unref);
}

/* commit callback */
static void _fcitx_im_context_commit_string_cb(FcitxClient *im,
                                               const char *str,
                                               void *user_data) {
    FCITX_UNUSED(im);
    FcitxLog(DEBUG, "_fcitx_im_context_commit_string_cb");
    FcitxIMContext *context = FCITX_IM_CONTEXT(user_data);
    g_signal_emit(context, context->signal_commit_id, 0, str);
}

/* enable/close im callbacks */
static void _fcitx_im_context_enable_im_cb(FcitxClient *im, void *user_data) {
    FCITX_UNUSED(im);
    FCITX_UNUSED(user_data);
}

static void _fcitx_im_context_close_im_cb(FcitxClient *im, void *user_data) {
    FCITX_UNUSED(im);
    FcitxLog(DEBUG, "_fcitx_im_context_close_im_cb");
    FcitxIMContext *context = FCITX_IM_CONTEXT(user_data);

    if (context->preedit_string != NULL) {
        g_free(context->preedit_string);
    }
    context->preedit_string = NULL;
    context->cursor_pos = 0;
    context->preedit_visible = FALSE;
    g_signal_emit(context, context->signal_preedit_changed_id, 0);
    g_signal_emit(context, context->signal_preedit_end_id, 0);
}

/* formatted preedit callback: GPtrArray of FcitxPreeditItem + cursor_pos */
static void _fcitx_im_context_update_formatted_preedit_cb(FcitxClient *im,
                                                          GPtrArray *array,
                                                          int cursor_pos,
                                                          void *user_data) {
    FCITX_UNUSED(im);
    FcitxLog(DEBUG, "_fcitx_im_context_update_formatted_preedit_cb");
    FcitxIMContext *context = FCITX_IM_CONTEXT(user_data);

    if (cursor_pos < 0) {
        cursor_pos = 0;
    }

    if (context->preedit_string != NULL) {
        g_free(context->preedit_string);
        context->preedit_string = NULL;
    }

    if (context->attrlist != NULL) {
        pango_attr_list_unref(context->attrlist);
    }

    context->attrlist = pango_attr_list_new();

    GString *gstr = g_string_new(NULL);

    unsigned int i = 0;
    for (i = 0; i < array->len; i++) {
        size_t bytelen = strlen(gstr->str);
        FcitxPreeditItem *preedit = (FcitxPreeditItem *)g_ptr_array_index(array, i);
        const gchar *s = preedit->string;
        gint type = preedit->type;

        PangoAttribute *pango_attr = NULL;
        if ((type & MSG_NOUNDERLINE) == 0) {
            pango_attr = pango_attr_underline_new(PANGO_UNDERLINE_SINGLE);
            pango_attr->start_index = bytelen;
            pango_attr->end_index = bytelen + strlen(s);
            pango_attr_list_insert(context->attrlist, pango_attr);
        }

        if (type & MSG_HIGHLIGHT) {
            gboolean hasColor = FALSE;
            GdkRGBA fg = {0, 0, 0, 1};
            GdkRGBA bg = {1, 1, 1, 1};

            if (context->client_widget) {
                GtkWidget *widget = context->client_widget;
                // black list window, otherwise firefox may gives you strange
                // color.
                if (GTK_IS_WIDGET(widget) && !GTK_IS_WINDOW(widget)) {
                    GtkStyleContext *styleContext =
                        gtk_widget_get_style_context(widget);
                    hasColor =
                        gtk_style_context_lookup_color(
                            styleContext, "theme_selected_bg_color", &bg) &&
                        gtk_style_context_lookup_color(
                            styleContext, "theme_selected_fg_color", &fg);
                }
            }

            if (hasColor) {
                pango_attr = pango_attr_background_new(
                    bg.red * 65535, bg.green * 65535, bg.blue * 65535);
                pango_attr->start_index = bytelen;
                pango_attr->end_index = bytelen + strlen(s);
                pango_attr_list_insert(context->attrlist, pango_attr);

                pango_attr = pango_attr_foreground_new(
                    fg.red * 65535, fg.green * 65535, fg.blue * 65535);
                pango_attr->start_index = bytelen;
                pango_attr->end_index = bytelen + strlen(s);
                pango_attr_list_insert(context->attrlist, pango_attr);
            }
        }

        g_string_append(gstr, s);
    }

    context->preedit_string = g_string_free(gstr, FALSE);
    context->cursor_pos = cursor_pos;

    /* 记录上一次可见性，用于翻转检测(B5: preedit-start/preedit-end 时序) */
    gboolean was_visible = context->preedit_visible;
    gboolean new_visible =
        (context->preedit_string != NULL && context->preedit_string[0] != 0);

    if (new_visible) {
        if (!was_visible) {
            /* invisible => visible: 先发 preedit-start */
            g_signal_emit(context, context->signal_preedit_start_id, 0);
            if (context->attrlist != NULL) {
                pango_attr_list_unref(context->attrlist);
                context->attrlist = NULL;
            }
        }
        g_signal_emit(context, context->signal_preedit_changed_id, 0);
    } else {
        if (was_visible) {
            /* visible => invisible: changed + end */
            g_signal_emit(context, context->signal_preedit_changed_id, 0);
            g_signal_emit(context, context->signal_preedit_end_id, 0);
        }
        /* still invisible: do nothing */
        if (context->attrlist != NULL) {
            pango_attr_list_unref(context->attrlist);
            context->attrlist = NULL;
        }
    }
    context->preedit_visible = new_visible;
}

/* destroy */
void fcitx_im_context_compose_cache_clear(void) {
    G_LOCK(compose_lock);
    if (compose_data_list != NULL) {
        g_list_free_full(compose_data_list,
                         (GDestroyNotify)_compose_data_free);
        compose_data_list = NULL;
    }
    G_UNLOCK(compose_lock);
}

static void fcitx_im_context_finalize(GObject *object) {
    FcitxIMContext *context = FCITX_IM_CONTEXT(object);

    if (context->forward_pending != NULL) {
        g_queue_free_full(context->forward_pending, _forward_key_item_free);
        context->forward_pending = NULL;
    }
    g_clear_pointer(&context->reinjected_events, g_hash_table_unref);
    g_clear_pointer(&context->handled_events_list, g_queue_free);
    g_clear_pointer(&context->pending_events, g_hash_table_unref);
    g_clear_pointer(&context->handled_events, g_hash_table_unref);

    if (_focus_im_context == (GtkIMContext *)context)
        _focus_im_context = NULL;
    if (context->surrounding_weak_self)
        g_object_remove_weak_pointer((GObject *)context,
                                     (gpointer *)&context->surrounding_weak_self);

    if (context->client) {
        g_signal_handlers_disconnect_by_data(context->client, context);
        g_object_unref(context->client);
        context->client = NULL;
    }

    if (context->client_widget) {
        g_object_unref(context->client_widget);
        context->client_widget = NULL;
    }
    if (context->slave) {
        g_object_unref(context->slave);
        context->slave = NULL;
    }
    if (context->preedit_string) {
        g_free(context->preedit_string);
        context->preedit_string = NULL;
    }
    if (context->surrounding_text) {
        g_free(context->surrounding_text);
        context->surrounding_text = NULL;
    }
    if (context->attrlist) {
        pango_attr_list_unref(context->attrlist);
        context->attrlist = NULL;
    }

    G_OBJECT_CLASS(fcitx_im_context_parent_class)->finalize(object);
}
