/*
 * fcitxim.c - GTK4 immodule entry point for fcitx4
 *
 * GTK4 loads input method modules as GIO extension point modules.
 * The module must export g_io_im_<name>_load() / g_io_im_<name>_unload()
 * which register a GType derived from GtkIMContext at the extension point
 * GTK_IM_MODULE_EXTENSION_POINT_NAME.
 *
 * Based on fcitx-4.2.9.8 gtk2/gtk3 frontend
 * Copyright (C) 2010~2012 CSSlayer <wengxt@gmail.com>
 * Copyright (C) 2026 ipfgao <ipfgao@hotmail.com>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <gio/gio.h>
#include <gtk/gtk.h>

#include "fcitximcontext.h"

G_MODULE_EXPORT void g_io_im_fcitx4_load(GIOModule *module)
{
    g_type_module_use(G_TYPE_MODULE(module));
    fcitx_im_context_register_type(G_TYPE_MODULE(module));

    g_io_extension_point_implement(GTK_IM_MODULE_EXTENSION_POINT_NAME,
                                   FCITX_TYPE_IM_CONTEXT, "fcitx", 10);
    /* 同时注册 "fcitx4"，使 GTK_IM_MODULE=fcitx4 也能匹配到本模块 */
    g_io_extension_point_implement(GTK_IM_MODULE_EXTENSION_POINT_NAME,
                                   FCITX_TYPE_IM_CONTEXT4, "fcitx4", 10);
}

G_MODULE_EXPORT void g_io_im_fcitx4_unload(GIOModule *module)
{
    fcitx_im_context_compose_cache_clear();
    g_type_module_unuse(G_TYPE_MODULE(module));
}
