/*
 * fcitx4 GTK4 IM module - header
 * Based on fcitx-4.2.9.8 gtk2/gtk3 frontend fcitximcontext.h
 * Copyright (C) 2010~2012 CSSlayer <wengxt@gmail.com>
 * Copyright (C) 2026 ipfgao <ipfgao@hotmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __FCITX_IM_CONTEXT_H__
#define __FCITX_IM_CONTEXT_H__

#include <gtk/gtk.h>
#include <gtk/gtkimmodule.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <fcitx-gclient/fcitxclient.h>

G_BEGIN_DECLS

#define FCITX_TYPE_IM_CONTEXT (fcitx_im_context_register_type(NULL))
#define FCITX_IM_CONTEXT(obj)                                                  \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), FCITX_TYPE_IM_CONTEXT, FcitxIMContext))
#define FCITX_IS_IM_CONTEXT(obj)                                               \
    (G_TYPE_CHECK_INSTANCE_TYPE((obj), FCITX_TYPE_IM_CONTEXT))
#define FCITX_IM_CONTEXT_CLASS(klass)                                          \
    (G_TYPE_CHECK_CLASS_CAST((klass), FCITX_TYPE_IM_CONTEXT,                   \
                             FcitxIMContextClass))
#define FCITX_IM_CONTEXT_GET_CLASS(obj)                                        \
    (G_TYPE_INSTANCE_GET_CLASS((obj), FCITX_TYPE_IM_CONTEXT,                   \
                               FcitxIMContextClass))

typedef struct _FcitxIMContext FcitxIMContext;
typedef struct _FcitxIMContextClass FcitxIMContextClass;

enum _fcitx_key_event_type {
    FcitxKeyEventTypePressKey = 0,
    FcitxKeyEventTypeReleaseKey = 1
};

struct _FcitxIMContext {
    GtkIMContext parent;
    GtkIMContext *slave;

    gboolean use_preedit;

    /* GTK4: we store the client widget (GtkWidget), not GdkWindow */
    GtkWidget *client_widget;

    GdkRectangle area;

    gboolean has_focus;
    gboolean support_surrounding_text;
    gboolean is_wayland;
    gboolean preedit_visible;

    /* 弱引用的安全存储点（避免指向栈变量） */
    FcitxIMContext *surrounding_weak_self;

    /* GTK4 无法构造 GdkKeyEvent：fcitx forward-key 转为待转发队列，
       在 filter_keypress 时匹配并交给 slave 处理 */
    GQueue *forward_pending;

    /* 重注入事件集合：ret==FALSE 时 put 回队列的事件在此登记，
       filter_keypress 入口匹配即移除并 return FALSE 交给 GTK 派发 */
    GHashTable *reinjected_events;

    char *preedit_string;
    char *surrounding_text;
    int cursor_pos;
    int last_cursor_pos;
    int last_anchor_pos;
    PangoAttrList *attrlist;

    gboolean sync_mode;

    FcitxClient *client;

    guint signal_commit_id;
    guint signal_retrieve_surrounding_id;
    guint signal_delete_surrounding_id;
    guint signal_preedit_changed_id;
    guint signal_preedit_start_id;
    guint signal_preedit_end_id;

    /* GTK4: event loop prevention tables (fcitx5-gtk4 style) */
    GHashTable *pending_events;
    GHashTable *handled_events;
    GQueue *handled_events_list;

    guint32 time;

    struct xkb_compose_state *xkbComposeState;
};

struct _FcitxIMContextClass {
    GtkIMContextClass parent_class;
};

GType fcitx_im_context_register_type(GTypeModule *type_module);
void fcitx_im_context_compose_cache_clear(void);
FcitxIMContext *fcitx_im_context_new(void);

G_END_DECLS

#endif /* __FCITX_IM_CONTEXT_H__ */
